// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/automation.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <sstream>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		// A drag moves the mouse in this many steps, so that elements on the way see it pass.
		constexpr int g_dragSteps = 8;

		std::string lowercase(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}

		// Text as it shows with white-space: normal: runs of white space as one space, none at the ends.
		std::string collapsed(const std::string& text)
		{
			std::string result;
			bool space = false;
			for (const char c : text)
			{
				if (std::isspace(static_cast<unsigned char>(c)) != 0)
				{
					space = !result.empty();
					continue;
				}
				if (space)
					result += ' ';
				space = false;
				result += c;
			}
			return result;
		}

		// The text of the element and its descendants.
		void collectText(Rml::Element& element, std::string& text)
		{
			if (const auto* node = rmlui_dynamic_cast<Rml::ElementText*>(&element))
				text += node->GetText() + ' ';
			for (int i = 0; i < element.GetNumChildren(); ++i)
				collectText(*element.GetChild(i), text);
		}

		bool visible(Rml::Element& element)
		{
			const Rml::Vector2f size = element.GetBox().GetSize(Rml::BoxArea::Border);
			return element.IsVisible(true) && size.x > 0.0F && size.y > 0.0F;
		}

		Rect box(Rml::Element& element)
		{
			const Rml::Vector2f offset = element.GetAbsoluteOffset(Rml::BoxArea::Border);
			const Rml::Vector2f size = element.GetBox().GetSize(Rml::BoxArea::Border);
			return {static_cast<int>(std::lround(offset.x)), static_cast<int>(std::lround(offset.y)),
				static_cast<int>(std::lround(size.x)), static_cast<int>(std::lround(size.y))};
		}

		Json node(Rml::Element& element, int depth)
		{
			if (const auto* text = rmlui_dynamic_cast<Rml::ElementText*>(&element))
				return {{"text", collapsed(text->GetText())}};

			Json result = {{"tag", element.GetTagName()}};
			if (!element.GetId().empty())
				result["id"] = element.GetId();
			std::istringstream classNames(element.GetClassNames());
			Json classes = Json::array();
			for (std::string name; classNames >> name;)
				classes.push_back(name);
			if (!classes.empty())
				result["classes"] = std::move(classes);
			Json attributes = Json::object();
			for (const auto& [name, value] : element.GetAttributes())
				if (name != "id" && name != "class")
					attributes[name] = value.Get<Rml::String>();
			if (!attributes.empty())
				result["attributes"] = std::move(attributes);
			if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(&element))
				result["value"] = control->GetValue();
			if (const Rml::StringList pseudoClasses = element.GetActivePseudoClasses(); !pseudoClasses.empty())
				result["pseudoClasses"] = pseudoClasses;
			result["visible"] = visible(element);
			const Rect bounds = box(element);
			result["box"] = {{"x", bounds.x}, {"y", bounds.y}, {"width", bounds.width}, {"height", bounds.height}};

			if (depth == 0)
			{
				std::string text;
				collectText(element, text);
				if (!collapsed(text).empty())
					result["text"] = collapsed(text);
				return result;
			}
			Json children = Json::array();
			for (int i = 0; i < element.GetNumChildren(); ++i)
			{
				Json child = node(*element.GetChild(i), depth - 1);
				if (child.value("text", std::string(" ")).empty() && !child.contains("tag"))
					continue; // white space between elements
				children.push_back(std::move(child));
			}
			if (!children.empty())
				result["children"] = std::move(children);
			return result;
		}

		Json nodeOrNull(Rml::Element* element)
		{
			return element != nullptr ? node(*element, 0) : Json();
		}

		// The elements that match the selector in the context's documents, documents included, in document order.
		Rml::ElementList matches(Rml::Context& context, const std::string& selector)
		{
			Rml::ElementList found;
			for (int i = 0; i < context.GetNumDocuments(); ++i)
			{
				Rml::ElementDocument* document = context.GetDocument(i);
				if (document->Matches(selector))
					found.push_back(document);
				Rml::ElementList inside;
				document->QuerySelectorAll(inside, selector);
				found.insert(found.end(), inside.begin(), inside.end());
			}
			return found;
		}

		int modifier(const std::string& name)
		{
			static const std::map<std::string, int> modifiers = {{"ctrl", Rml::Input::KM_CTRL},
				{"control", Rml::Input::KM_CTRL}, {"shift", Rml::Input::KM_SHIFT}, {"alt", Rml::Input::KM_ALT},
				{"option", Rml::Input::KM_ALT}, {"meta", Rml::Input::KM_META}, {"cmd", Rml::Input::KM_META},
				{"command", Rml::Input::KM_META}};
			const auto it = modifiers.find(lowercase(name));
			if (it == modifiers.end())
				throw std::invalid_argument("unknown modifier " + name + "; use Ctrl, Shift, Alt or Meta");
			return it->second;
		}

		int modifierState(const std::vector<std::string>& names)
		{
			int state = 0;
			for (const std::string& name : names)
				state |= modifier(name);
			return state;
		}

		Rml::Input::KeyIdentifier keyIdentifier(const std::string& name)
		{
			using namespace Rml::Input;
			static const std::map<std::string, KeyIdentifier> named = {{"enter", KI_RETURN}, {"return", KI_RETURN},
				{"escape", KI_ESCAPE}, {"esc", KI_ESCAPE}, {"tab", KI_TAB}, {"backspace", KI_BACK},
				{"delete", KI_DELETE}, {"del", KI_DELETE}, {"insert", KI_INSERT}, {"home", KI_HOME}, {"end", KI_END},
				{"pageup", KI_PRIOR}, {"pagedown", KI_NEXT}, {"up", KI_UP}, {"down", KI_DOWN}, {"left", KI_LEFT},
				{"right", KI_RIGHT}, {"space", KI_SPACE}};
			const std::string key = lowercase(name);
			if (const auto it = named.find(key); it != named.end())
				return it->second;
			if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z')
				return static_cast<KeyIdentifier>(KI_A + (key[0] - 'a'));
			if (key.size() == 1 && key[0] >= '0' && key[0] <= '9')
				return static_cast<KeyIdentifier>(KI_0 + (key[0] - '0'));
			if (key.size() > 1 && key[0] == 'f' &&
				std::all_of(key.begin() + 1, key.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
				if (const int number = std::stoi(key.substr(1)); number >= 1 && number <= 24)
					return static_cast<KeyIdentifier>(KI_F1 + number - 1);
			throw std::invalid_argument("unknown key " + name);
		}

		int buttonIndex(MouseButton button)
		{
			switch (button)
			{
			case MouseButton::Right:
				return 1;
			case MouseButton::Middle:
				return 2;
			default:
				return 0;
			}
		}

		// The window's context, laid out as it is now.
		Rml::Context& updated(Window& window)
		{
			window.context().Update();
			return window.context();
		}

		Rml::Element& targetElement(Window& window, const Target& target)
		{
			const Rml::ElementList found = matches(updated(window), target.selector);
			if (found.empty())
				throw std::invalid_argument("no element matches " + target.selector);
			if (target.index < 0 || static_cast<size_t>(target.index) >= found.size())
				throw std::invalid_argument("only " + std::to_string(found.size()) +
					(found.size() == 1 ? " element matches " : " elements match ") + target.selector);
			Rml::Element& chosen = *found[static_cast<size_t>(target.index)];
			if (!visible(chosen))
				throw std::invalid_argument(target.selector + " is not visible");
			return chosen;
		}

		Point targetPoint(Window& window, const Target& target)
		{
			if (target.point)
				return *target.point;
			if (target.selector.empty())
				throw std::invalid_argument("needs a selector or x and y");
			const Rect bounds = box(targetElement(window, target));
			return {bounds.x + (bounds.width / 2), bounds.y + (bounds.height / 2)};
		}

		// The element under the mouse after input, which the window draws again.
		Json under(Window& window)
		{
			window.requestRender();
			return {{"element", nodeOrNull(window.context().GetHoverElement())}};
		}
	}

	Window& Automation::resolve(std::optional<WindowId> window)
	{
		if (window)
		{
			if (Window* found = m_shell.find(*window))
				return *found;
			throw std::invalid_argument("no window " + std::to_string(*window) + "; ui_windows lists them");
		}
		for (Window* open : m_shell.windows())
			if (open->main())
				return *open;
		throw std::runtime_error("no window is open");
	}

	Json Automation::windows()
	{
		Json list = Json::array();
		for (const Window* open : m_shell.windows())
		{
			Rml::Context& context = open->context();
			Json documents = Json::array();
			for (int i = 0; i < context.GetNumDocuments(); ++i)
			{
				const Rml::ElementDocument* document = context.GetDocument(i);
				// RmlUi writes a drive letter's colon as '|'.
				const std::string source = Rml::StringUtilities::Replace(document->GetSourceURL(), '|', ':');
				documents.push_back({{"title", document->GetTitle()}, {"source", source},
					{"visible", document->IsVisible()}, {"modal", document->IsModal()}});
			}
			list.push_back({{"id", open->id()}, {"main", open->main()}, {"width", open->pixelSize().width},
				{"height", open->pixelSize().height}, {"dpRatio", open->dpRatio()},
				{"documents", std::move(documents)}});
		}
		return {{"windows", std::move(list)}};
	}

	Json Automation::dom(std::optional<WindowId> window, const std::string& selector, int depth)
	{
		Rml::Context& context = updated(resolve(window));
		Json elements = Json::array();
		if (selector.empty())
		{
			for (int i = 0; i < context.GetNumDocuments(); ++i)
				elements.push_back(node(*context.GetDocument(i), depth));
		}
		else
		{
			for (Rml::Element* found : matches(context, selector))
				elements.push_back(node(*found, depth));
		}
		return {{"elements", std::move(elements)}};
	}

	Image Automation::screenshot(std::optional<WindowId> window, const Target& element)
	{
		Window& shown = resolve(window);
		std::optional<Image> image = m_shell.screenshot(shown.id());
		if (!image)
			throw std::runtime_error("the window can't be drawn offscreen");
		if (element.empty())
			return *image;

		const Rect bounds = box(targetElement(shown, element));
		const int left = std::clamp(bounds.x, 0, image->width);
		const int top = std::clamp(bounds.y, 0, image->height);
		const int right = std::clamp(bounds.x + bounds.width, left, image->width);
		const int bottom = std::clamp(bounds.y + bounds.height, top, image->height);
		Image cropped{right - left, bottom - top, {}};
		cropped.pixels.reserve(static_cast<size_t>(cropped.width) * static_cast<size_t>(cropped.height) * 4);
		for (int row = top; row < bottom; ++row)
		{
			const auto begin = image->pixels.begin() + (((static_cast<ptrdiff_t>(row) * image->width) + left) * 4);
			cropped.pixels.insert(cropped.pixels.end(), begin, begin + (static_cast<ptrdiff_t>(cropped.width) * 4));
		}
		return cropped;
	}

	Json Automation::move(std::optional<WindowId> window, const Target& to)
	{
		Window& shown = resolve(window);
		const Point at = targetPoint(shown, to);
		updated(shown).ProcessMouseMove(at.x, at.y, 0);
		return under(shown);
	}

	Json Automation::click(std::optional<WindowId> window, const Target& target, MouseButton button, int count,
		const std::vector<std::string>& modifiers)
	{
		Window& shown = resolve(window);
		const Point at = targetPoint(shown, target);
		const int state = modifierState(modifiers);
		Rml::Context& context = updated(shown);
		context.ProcessMouseMove(at.x, at.y, state);
		for (int i = 0; i < count; ++i)
		{
			context.ProcessMouseButtonDown(buttonIndex(button), state);
			context.ProcessMouseButtonUp(buttonIndex(button), state);
		}
		return under(shown);
	}

	Json Automation::drag(std::optional<WindowId> window, const Target& from, const Target& to)
	{
		Window& shown = resolve(window);
		const Point start = targetPoint(shown, from);
		const Point end = targetPoint(shown, to);
		Rml::Context& context = updated(shown);
		context.ProcessMouseMove(start.x, start.y, 0);
		context.ProcessMouseButtonDown(0, 0);
		for (int step = 1; step <= g_dragSteps; ++step)
			context.ProcessMouseMove(start.x + ((end.x - start.x) * step / g_dragSteps),
				start.y + ((end.y - start.y) * step / g_dragSteps), 0);
		context.ProcessMouseButtonUp(0, 0);
		return under(shown);
	}

	Json Automation::wheel(std::optional<WindowId> window, const Target& at, float dx, float dy)
	{
		Window& shown = resolve(window);
		Rml::Context& context = updated(shown);
		if (!at.empty())
		{
			const Point point = targetPoint(shown, at);
			context.ProcessMouseMove(point.x, point.y, 0);
		}
		context.ProcessMouseWheel(Rml::Vector2f(dx, dy), 0);
		return under(shown);
	}

	Json Automation::key(std::optional<WindowId> window, const std::string& keys)
	{
		std::vector<std::string> parts;
		std::istringstream split(keys);
		for (std::string part; std::getline(split, part, '+');)
			parts.push_back(part);
		if (parts.empty())
			throw std::invalid_argument("no key given");
		const Rml::Input::KeyIdentifier pressed = keyIdentifier(parts.back());
		parts.pop_back();
		const int state = modifierState(parts);

		Window& shown = resolve(window);
		Rml::Context& context = updated(shown);
		context.ProcessKeyDown(pressed, state);
		context.ProcessKeyUp(pressed, state);
		shown.requestRender();
		return {{"focus", nodeOrNull(context.GetFocusElement())}};
	}

	Json Automation::text(std::optional<WindowId> window, const std::string& text)
	{
		Window& shown = resolve(window);
		Rml::Context& context = updated(shown);
		context.ProcessTextInput(text);
		shown.requestRender();
		return {{"focus", nodeOrNull(context.GetFocusElement())}};
	}
}
