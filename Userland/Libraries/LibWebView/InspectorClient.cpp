/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/StringBuilder.h>
#include <LibCore/Resource.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWebView/InspectorClient.h>
#include <LibWebView/SourceHighlighter.h>

namespace WebView {

static ErrorOr<JsonValue> parse_json_tree(StringView json)
{
    auto parsed_tree = TRY(JsonValue::from_string(json));
    if (!parsed_tree.is_object())
        return Error::from_string_literal("Expected tree to be a JSON object");

    return parsed_tree;
}

InspectorClient::InspectorClient(ViewImplementation& content_web_view, ViewImplementation& inspector_web_view)
    : m_content_web_view(content_web_view)
    , m_inspector_web_view(inspector_web_view)
{
    m_content_web_view.on_received_dom_tree = [this](auto const& dom_tree) {
        auto result = parse_json_tree(dom_tree);
        if (result.is_error()) {
            dbgln("Failed to load DOM tree: {}", result.error());
            return;
        }

        auto dom_tree_html = generate_dom_tree(result.value().as_object());
        auto dom_tree_base64 = MUST(encode_base64(dom_tree_html.bytes()));

        auto script = MUST(String::formatted("inspector.loadDOMTree(\"{}\");", dom_tree_base64));
        m_inspector_web_view.run_javascript(script);

        m_dom_tree_loaded = true;

        if (m_pending_selection.has_value())
            select_node(m_pending_selection.release_value());
        else
            select_default_node();
    };

    m_content_web_view.on_received_accessibility_tree = [this](auto const& accessibility_tree) {
        auto result = parse_json_tree(accessibility_tree);
        if (result.is_error()) {
            dbgln("Failed to load accessibility tree: {}", result.error());
            return;
        }

        auto accessibility_tree_html = generate_accessibility_tree(result.value().as_object());
        auto accessibility_tree_base64 = MUST(encode_base64(accessibility_tree_html.bytes()));

        auto script = MUST(String::formatted("inspector.loadAccessibilityTree(\"{}\");", accessibility_tree_base64));
        m_inspector_web_view.run_javascript(script);
    };

    m_inspector_web_view.enable_inspector_prototype();
    m_inspector_web_view.use_native_user_style_sheet();

    m_inspector_web_view.on_inspector_loaded = [this]() {
        inspect();
    };

    m_inspector_web_view.on_inspector_selected_dom_node = [this](auto node_id, auto const& pseudo_element) {
        auto inspected_node_properties = m_content_web_view.inspect_dom_node(node_id, pseudo_element);

        StringBuilder builder;

        // FIXME: Support box model metrics and ARIA properties.
        auto generate_property_script = [&](auto const& computed_style, auto const& resolved_style, auto const& custom_properties) {
            builder.append("inspector.createPropertyTables(\""sv);
            builder.append_escaped_for_json(computed_style);
            builder.append("\", \""sv);
            builder.append_escaped_for_json(resolved_style);
            builder.append("\", \""sv);
            builder.append_escaped_for_json(custom_properties);
            builder.append("\");"sv);
        };

        if (inspected_node_properties.is_error()) {
            generate_property_script("{}"sv, "{}"sv, "{}"sv);
        } else {
            generate_property_script(
                inspected_node_properties.value().computed_style_json,
                inspected_node_properties.value().resolved_style_json,
                inspected_node_properties.value().custom_properties_json);
        }

        m_inspector_web_view.run_javascript(builder.string_view());
    };

    load_inspector();
}

InspectorClient::~InspectorClient()
{
    m_content_web_view.on_received_dom_tree = nullptr;
    m_content_web_view.on_received_accessibility_tree = nullptr;
}

void InspectorClient::inspect()
{
    m_content_web_view.inspect_dom_tree();
    m_content_web_view.inspect_accessibility_tree();
}

void InspectorClient::reset()
{
    m_body_node_id.clear();
    m_pending_selection.clear();

    m_dom_tree_loaded = false;
}

void InspectorClient::select_hovered_node()
{
    auto hovered_node_id = m_content_web_view.get_hovered_node_id();
    select_node(hovered_node_id);
}

void InspectorClient::select_default_node()
{
    if (m_body_node_id.has_value())
        select_node(*m_body_node_id);
}

void InspectorClient::clear_selection()
{
    m_content_web_view.clear_inspected_dom_node();

    static constexpr auto script = "inspector.clearInspectedDOMNode();"sv;
    m_inspector_web_view.run_javascript(script);
}

void InspectorClient::select_node(i32 node_id)
{
    if (!m_dom_tree_loaded) {
        m_pending_selection = node_id;
        return;
    }

    auto script = MUST(String::formatted("inspector.inspectDOMNodeID({});", node_id));
    m_inspector_web_view.run_javascript(script);
}

void InspectorClient::load_inspector()
{
    StringBuilder builder;

    // FIXME: Teach LibWeb how to load resource:// URIs instead of needing to read these files here.
    auto inspector_css = MUST(Core::Resource::load_from_uri("resource://html/inspector/inspector.css"sv));
    auto inspector_js = MUST(Core::Resource::load_from_uri("resource://html/inspector/inspector.js"sv));

    builder.append(R"~~~(
<!DOCTYPE html>
<html>
<head>
    <meta name="color-scheme" content="dark light">
    <style type="text/css">
)~~~"sv);

    builder.append(HTML_HIGHLIGHTER_STYLE);
    builder.append(inspector_css->data());

    builder.append(R"~~~(
    </style>
</head>
<body>
    <div class="split-view">
        <div id="inspector-top" class="split-view-container" style="height: 60%">
            <div class="tab-controls-container">
                <div class="tab-controls">
                    <button id="dom-tree-button" onclick="selectTopTab(this, 'dom-tree')">DOM Tree</button>
                    <button id="accessibility-tree-button" onclick="selectTopTab(this, 'accessibility-tree')">Accessibility Tree</button>
                </div>
            </div>
            <div id="dom-tree" class="tab-content html"></div>
            <div id="accessibility-tree" class="tab-content"></div>
        </div>
        <div id="inspector-bottom" class="split-view-container" style="height: 40%">
            <div class="tab-controls-container">
                <div class="tab-controls">
                    <button id="computed-style-button" onclick="selectBottomTab(this, 'computed-style')">Computed Style</button>
                    <button id="resolved-style-button" onclick="selectBottomTab(this, 'resolved-style')">Resolved Style</button>
                    <button id="custom-properties-button" onclick="selectBottomTab(this, 'custom-properties')">Custom Properties</button>
                </div>
            </div>
)~~~"sv);

    auto generate_property_table = [&](auto name) {
        builder.appendff(R"~~~(
            <div id="{0}" class="tab-content">
                <table class="property-table">
                    <thead>
                        <tr>
                            <th>Name</th>
                            <th>Value</th>
                        </tr>
                    </thead>
                    <tbody id="{0}-table">
                    </tbody>
                </table>
            </div>
)~~~",
            name);
    };

    generate_property_table("computed-style"sv);
    generate_property_table("resolved-style"sv);
    generate_property_table("custom-properties"sv);

    builder.append(R"~~~(
        </div>
    </div>

    <script type="text/javascript">
)~~~"sv);

    builder.append(inspector_js->data());

    builder.append(R"~~~(
    </script>
</body>
</html>
)~~~"sv);

    m_inspector_web_view.load_html(builder.string_view());
}

template<typename Generator>
static void generate_tree(StringBuilder& builder, JsonObject const& node, Generator&& generator)
{
    if (auto children = node.get_array("children"sv); children.has_value() && !children->is_empty()) {
        auto name = node.get_deprecated_string("name"sv).value_or({});
        builder.append("<details>"sv);

        builder.append("<summary>"sv);
        generator(node);
        builder.append("</summary>"sv);

        children->for_each([&](auto const& child) {
            builder.append("<div>"sv);
            generate_tree(builder, child.as_object(), generator);
            builder.append("</div>"sv);
        });

        builder.append("</details>"sv);
    } else {
        generator(node);
    }
}

String InspectorClient::generate_dom_tree(JsonObject const& dom_tree)
{
    StringBuilder builder;

    generate_tree(builder, dom_tree, [&](JsonObject const& node) {
        auto type = node.get_deprecated_string("type"sv).value_or("unknown"sv);
        auto name = node.get_deprecated_string("name"sv).value_or({});

        StringBuilder data_attributes;
        auto append_data_attribute = [&](auto name, auto value) {
            if (!data_attributes.is_empty())
                data_attributes.append(' ');
            data_attributes.appendff("data-{}=\"{}\"", name, value);
        };

        if (auto pseudo_element = node.get_integer<i32>("pseudo-element"sv); pseudo_element.has_value()) {
            append_data_attribute("id"sv, node.get_integer<i32>("parent-id"sv).value());
            append_data_attribute("pseudo-element"sv, *pseudo_element);
        } else {
            append_data_attribute("id"sv, node.get_integer<i32>("id"sv).value());
        }

        if (type == "text"sv) {
            auto deprecated_text = node.get_deprecated_string("text"sv).release_value();
            deprecated_text = escape_html_entities(deprecated_text);

            if (auto text = MUST(Web::Infra::strip_and_collapse_whitespace(deprecated_text)); text.is_empty()) {
                builder.appendff("<span class=\"hoverable internal\" {}>", data_attributes.string_view());
                builder.append(name);
                builder.append("</span>"sv);
            } else {
                builder.appendff("<span class=\"hoverable\" {}>", data_attributes.string_view());
                builder.append(text);
                builder.append("</span>"sv);
            }

            return;
        }

        if (type == "comment"sv) {
            auto comment = node.get_deprecated_string("data"sv).release_value();
            comment = escape_html_entities(comment);

            builder.appendff("<span class=\"hoverable comment\" {}>", data_attributes.string_view());
            builder.appendff("&lt;!--{}--&gt;", comment);
            builder.append("</span>"sv);
            return;
        }

        if (type == "shadow-root"sv) {
            auto mode = node.get_deprecated_string("mode"sv).release_value();

            builder.appendff("<span class=\"hoverable internal\" {}>", data_attributes.string_view());
            builder.appendff("{} ({})", name, mode);
            builder.append("</span>"sv);
            return;
        }

        if (type != "element"sv) {
            builder.appendff("<span class=\"hoverable internal\" {}>", data_attributes.string_view());
            builder.appendff(name);
            builder.append("</span>"sv);
            return;
        }

        if (name.equals_ignoring_ascii_case("BODY"sv))
            m_body_node_id = node.get_integer<i32>("id"sv).value();

        builder.appendff("<span class=\"hoverable\" {}>", data_attributes.string_view());
        builder.append("<span>&lt;</span>"sv);
        builder.appendff("<span class=\"tag\">{}</span>", name.to_lowercase());

        if (auto attributes = node.get_object("attributes"sv); attributes.has_value()) {
            attributes->for_each_member([&builder](auto const& name, auto const& value) {
                builder.append("&nbsp;"sv);
                builder.appendff("<span class=\"attribute-name\">{}</span>", name);
                builder.append('=');
                builder.appendff("<span class=\"attribute-value\">\"{}\"</span>", value);
            });
        }

        builder.append("<span>&gt;</span>"sv);
        builder.append("</span>"sv);
    });

    return MUST(builder.to_string());
}

String InspectorClient::generate_accessibility_tree(JsonObject const& accessibility_tree)
{
    StringBuilder builder;

    generate_tree(builder, accessibility_tree, [&](JsonObject const& node) {
        auto type = node.get_deprecated_string("type"sv).value_or("unknown"sv);
        auto role = node.get_deprecated_string("role"sv).value_or({});

        if (type == "text"sv) {
            auto text = node.get_deprecated_string("text"sv).release_value();
            text = escape_html_entities(text);

            builder.appendff("<span class=\"hoverable\">");
            builder.append(MUST(Web::Infra::strip_and_collapse_whitespace(text)));
            builder.append("</span>"sv);
            return;
        }

        if (type != "element"sv) {
            builder.appendff("<span class=\"hoverable internal\">");
            builder.appendff(role.to_lowercase());
            builder.append("</span>"sv);
            return;
        }

        auto name = node.get_deprecated_string("name"sv).value_or({});
        auto description = node.get_deprecated_string("description"sv).value_or({});

        builder.appendff("<span class=\"hoverable\">");
        builder.append(role.to_lowercase());
        builder.appendff(" name: \"{}\", description: \"{}\"", name, description);
        builder.append("</span>"sv);
    });

    return MUST(builder.to_string());
}

}
