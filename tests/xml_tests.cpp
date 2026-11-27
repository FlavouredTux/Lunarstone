#include "framework.hpp"

#include <lunar/Xml.hpp>

#include <ranges>
#include <string>

using namespace lunar;

TEST(xml_parses_elements_attributes_and_text)
{
    auto doc = xml::parse(R"(<?xml version="1.0" encoding="utf-8"?>
<!-- leading comment -->
<root version='4' kind="test">
    <a>hello</a>
    <b x="1"/>
    <a>world</a>
</root>
)");
    CHECK(doc.has_value());
    CHECK_EQ(doc->name, "root");
    CHECK_EQ(*doc->attribute("version"), "4");
    CHECK_EQ(*doc->attribute("kind"), "test");
    CHECK(doc->attribute("missing") == nullptr);
    CHECK_EQ(doc->children.size(), 3u);
    CHECK_EQ(doc->child("a")->text, "hello");
    CHECK_EQ(*doc->child("b")->attribute("x"), "1");
    CHECK(doc->child("b")->children.empty());
    CHECK_EQ(std::ranges::distance(doc->childrenNamed("a")), 2);
}

TEST(xml_decodes_entities_and_cdata)
{
    auto doc = xml::parse(
        R"(<s a="&quot;q&quot;">&lt;tag&gt; &amp; &apos;x&apos; &#65;&#x42; &#233;<![CDATA[ <raw> & stuff ]]><!-- skipped --></s>)");
    CHECK(doc.has_value());
    CHECK_EQ(*doc->attribute("a"), "\"q\"");
    CHECK_EQ(doc->text, "<tag> & 'x' AB \xC3\xA9 <raw> & stuff ");
}

TEST(xml_keeps_whitespace_in_text)
{
    auto doc = xml::parse("<s>  two  spaces \n</s>");
    CHECK_EQ(doc->text, "  two  spaces \n");
}

TEST(xml_reports_errors_with_lines)
{
    auto mismatched = xml::parse("<a>\n<b>\n</c>\n</a>");
    CHECK(!mismatched.has_value());
    CHECK_EQ(mismatched.error().line, 3);
    CHECK_EQ(mismatched.error().message, "mismatched closing tag, expected </b>");

    auto unclosed = xml::parse("<a><b></b>");
    CHECK(!unclosed.has_value());
    CHECK_EQ(unclosed.error().message, "<a> is never closed");

    CHECK_EQ(xml::parse("<a>&nope;</a>").error().message, "unknown entity &nope;");
    CHECK_EQ(xml::parse("<a/><b/>").error().message, "unexpected content after the root element");
    CHECK_EQ(xml::parse("   ").error().message, "expected a root element");
    CHECK_EQ(xml::parse("<a x=1/>").error().message, "expected a quoted attribute value");
}
