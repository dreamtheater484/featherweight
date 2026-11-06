#include "TestHarness.h"

#include <string>

#include "pkg/Canonical.h"
#include "pkg/XmlDom.h"
#include "pkg/XmlPull.h"
#include "pkg/XmlWriter.h"

using namespace fw::pkg;
using Event = XmlPull::Event;

static Status::Code CheckCode(std::string_view xml, const Limits& limits = Limits::Default()) {
  return XmlPull::Check(xml, limits).code;
}

static std::string Canon(std::string_view xml) {
  std::string out;
  Status st = CanonicalizeXml(xml, out);
  return st.Ok() ? out : "ERROR: " + st.message;
}

// ---------- XmlPull ----------

TEST(XmlPull_EventsAndNamespaces) {
  XmlPull p(
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
      "<w:doc xmlns:w=\"urn:w\" xmlns=\"urn:default\" w:a=\"1\" b='2'>"
      "<w:p>text</w:p><plain/><inner xmlns=\"\"><x/></inner><!--note--><?pi data?></w:doc>");
  CHECK(p.Next() == Event::StartElement);
  CHECK(p.QName() == "w:doc" && p.LocalName() == "doc" && p.Prefix() == "w" && p.NamespaceUri() == "urn:w");
  CHECK_EQ(p.Depth(), 1);
  CHECK(p.FindAttribute("urn:w", "a") && p.FindAttribute("urn:w", "a")->value == "1");
  CHECK(p.FindAttribute("", "b") && p.FindAttribute("", "b")->value == "2");  // unprefixed: no namespace
  CHECK(p.Attributes()[0].IsNamespaceDecl());
  CHECK(p.Next() == Event::StartElement && p.QName() == "w:p");
  CHECK(p.Next() == Event::Text && p.Text() == "text");
  CHECK(p.Next() == Event::EndElement && p.QName() == "w:p" && p.NamespaceUri() == "urn:w");
  CHECK(p.Next() == Event::StartElement && p.QName() == "plain" && p.NamespaceUri() == "urn:default");
  CHECK(p.IsEmptyElement());
  CHECK(p.Next() == Event::EndElement && p.QName() == "plain");
  CHECK(p.Next() == Event::StartElement && p.NamespaceUri().empty());  // xmlns="" undeclares
  CHECK(p.Next() == Event::StartElement && p.QName() == "x" && p.NamespaceUri().empty() && p.Depth() == 3);
  CHECK(p.Next() == Event::EndElement);
  CHECK(p.Next() == Event::EndElement && p.QName() == "inner");
  CHECK(p.Next() == Event::Comment && p.Text() == "note");
  CHECK(p.Next() == Event::ProcessingInstruction && p.QName() == "pi" && p.Text() == "data");
  CHECK(p.Next() == Event::EndElement && p.QName() == "w:doc");
  CHECK(p.Next() == Event::EndDocument);
  CHECK(p.Next() == Event::EndDocument);
}

TEST(XmlPull_EntitiesAndLineEnds) {
  XmlPull p("<a t=\"x&amp;y&#9;&#x41;\tz\r\nw\">&lt;&gt;&amp;&apos;&quot;&#233;&#x1F600;\r\nline\rend</a>");
  CHECK(p.Next() == Event::StartElement);
  // Literal tabs and line ends in attributes become spaces; character references don't.
  CHECK(p.Attributes()[0].value == "x&y\tA z w");
  CHECK(p.Next() == Event::Text);
  CHECK(p.Text() == "<>&'\"\xC3\xA9\xF0\x9F\x98\x80\nline\nend");
}

TEST(XmlPull_CDataAndComments) {
  XmlPull p("<a><![CDATA[<b> & ]]\r\n]]><!-- a & b --></a>");
  CHECK(p.Next() == Event::StartElement);
  CHECK(p.Next() == Event::CData && p.Text() == "<b> & ]]\n");
  CHECK(p.Next() == Event::Comment && p.Text() == " a & b ");
}

TEST(XmlPull_Utf16) {
  std::u16string text = u"﻿<?xml version=\"1.0\" encoding=\"UTF-16\"?><a>café \U0001F600</a>";
  std::string bytes(reinterpret_cast<const char*>(text.data()), text.size() * 2);
  XmlPull p(bytes);
  CHECK(p.Next() == Event::StartElement);
  CHECK(p.Next() == Event::Text && p.Text() == "caf\xC3\xA9 \xF0\x9F\x98\x80");
  CHECK(XmlPull::Check(bytes).Ok());
  CHECK_EQ(CheckCode(std::string_view(bytes).substr(0, bytes.size() - 1)), Status::Code::Damaged);  // odd length
}

TEST(XmlPull_RejectsDtd) {
  Status st = XmlPull::Check("<?xml version=\"1.0\"?><!DOCTYPE a [<!ENTITY x \"y\">]><a>&x;</a>", Limits::Default(),
                             "word/document.xml");
  CHECK_EQ(st.code, Status::Code::Rejected);
  CHECK(st.message.find("word/document.xml") != std::string::npos);
  CHECK(st.message.find("DTD") != std::string::npos);
}

TEST(XmlPull_Limits) {
  Limits limits;
  limits.maxDepth = 3;
  CHECK(XmlPull::Check("<a><b><c/></b></a>", limits).Ok());
  CHECK_EQ(CheckCode("<a><b><c><d/></c></b></a>", limits), Status::Code::TooLarge);
  limits.maxStringLength = 8;
  CHECK_EQ(CheckCode("<a>123456789</a>", limits), Status::Code::TooLarge);
  CHECK_EQ(CheckCode("<a x=\"123456789\"/>", limits), Status::Code::TooLarge);
  CHECK_EQ(CheckCode("<a><!--123456789--></a>", limits), Status::Code::TooLarge);
  CHECK(XmlPull::Check("<a>12345678</a>", limits).Ok());
  Limits attrs;
  attrs.maxAttributes = 2;
  CHECK_EQ(CheckCode("<a x=\"1\" y=\"2\" z=\"3\"/>", attrs), Status::Code::TooLarge);
}

TEST(XmlPull_Malformed) {
  using C = Status::Code;
  CHECK_EQ(CheckCode(""), C::Damaged);
  CHECK_EQ(CheckCode("<a>"), C::Damaged);
  CHECK_EQ(CheckCode("<a></b>"), C::Damaged);
  CHECK_EQ(CheckCode("<a/><b/>"), C::Damaged);              // two roots
  CHECK_EQ(CheckCode("<a/>text"), C::Damaged);              // text after the root
  CHECK_EQ(CheckCode("<a x=\"1\" x=\"2\"/>"), C::Damaged);  // repeated attribute
  CHECK_EQ(CheckCode("<a xmlns:p=\"u\" xmlns:q=\"u\" p:x=\"1\" q:x=\"2\"/>"), C::Damaged);  // same expanded name
  CHECK_EQ(CheckCode("<p:a/>"), C::Damaged);                // undeclared prefix
  CHECK_EQ(CheckCode("<a xmlns:=\"u\"/>"), C::Damaged);      // not a namespace name
  CHECK_EQ(CheckCode("<a:b:c xmlns:a=\"u\"/>"), C::Damaged);
  CHECK_EQ(CheckCode("<a>&nbsp;</a>"), C::Damaged);         // no DTD, so no such entity
  CHECK_EQ(CheckCode("<a>&#0;</a>"), C::Damaged);           // not an XML character
  CHECK_EQ(CheckCode("<a>&#xD800;</a>"), C::Damaged);
  CHECK_EQ(CheckCode("<a x=\"<\"/>"), C::Damaged);
  CHECK_EQ(CheckCode("<a x=1/>"), C::Damaged);
  CHECK_EQ(CheckCode("<a><?xml version=\"1.0\"?></a>"), C::Damaged);  // declaration not at the start
  CHECK_EQ(CheckCode("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a/>"), C::Unsupported);
  CHECK_EQ(CheckCode("<?xml version=1.0?><a/>"), C::Damaged);  // the declaration is parsed strictly
  CHECK_EQ(CheckCode("<?xml 6ersion=\"1.0\"?><a/>"), C::Damaged);  // found by fuzzing
  CHECK_EQ(CheckCode("<?xml version=\"1.0\"standalone=\"yes\"?><a/>"), C::Damaged);
  CHECK(XmlPull::Check("\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<a/>\n<!--after-->\n").Ok());
  // Reserved prefixes and namespace names. The first was found by fuzzing: as a kept fragment,
  // the inner element gained xmlns="s", which then clashed with xmlns:xmlns.
  CHECK_EQ(CheckCode("<data xmlns='s'><data xmlns:xmlns='a'></data></data>"), C::Damaged);
  CHECK_EQ(CheckCode("<a xmlns:xml=\"urn:other\"/>"), C::Damaged);
  CHECK_EQ(CheckCode("<a xmlns:p=\"http://www.w3.org/XML/1998/namespace\"/>"), C::Damaged);
  CHECK_EQ(CheckCode("<a xmlns=\"http://www.w3.org/XML/1998/namespace\"/>"), C::Damaged);
  CHECK_EQ(CheckCode("<a xmlns:p=\"http://www.w3.org/2000/xmlns/\"/>"), C::Damaged);
  CHECK(XmlPull::Check("<a xmlns:xml=\"http://www.w3.org/XML/1998/namespace\" xml:space=\"preserve\"/>").Ok());
}

TEST(XmlPull_OnlyXmlCharactersInValidUtf8) {
  using C = Status::Code;
  // Found by fuzzing: a NUL in an attribute ended the text early for pugixml.
  CHECK_EQ(CheckCode(std::string_view("<a x=\"1\0\0\"/>", 13)), C::Damaged);
  CHECK_EQ(CheckCode("<a>\x01</a>"), C::Damaged);
  CHECK_EQ(CheckCode("<a>\xC3</a>"), C::Damaged);              // truncated sequence
  CHECK_EQ(CheckCode("<a>\xC0\xAF</a>"), C::Damaged);          // overlong
  CHECK_EQ(CheckCode("<a>\xED\xA0\x80</a>"), C::Damaged);      // a surrogate
  CHECK_EQ(CheckCode("<a>\xEF\xBF\xBE</a>"), C::Damaged);      // U+FFFE
  CHECK_EQ(CheckCode("<a>\xF4\x90\x80\x80</a>"), C::Damaged);  // past U+10FFFF
  CHECK(XmlPull::Check("<a>\t\r\n caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80 \xC2\x85</a>").Ok());
  Status st = XmlPull::Check("<a>\n\n<b>\x02</b></a>");
  CHECK(st.message.find("line 3") != std::string::npos);
}

TEST(XmlPull_ManyAttributesStayFast) {
  // The duplicate check must not be quadratic.
  std::string xml = "<a";
  for (int i = 0; i < 5000; ++i) xml += " a" + std::to_string(i) + "=\"v\"";
  xml += "/>";
  CHECK(XmlPull::Check(xml).Ok());
}

// ---------- XmlWriter ----------

TEST(XmlWriter_Escaping) {
  XmlWriter w;
  w.StartElement("a");
  w.Attribute("x", "<\"&>\t\n\r");
  w.StartElement("b");
  w.EndElement();
  w.Text("1 < 2 & 3 > 2\r\n");
  w.CData("x]]>y");
  w.EndElement();
  CHECK(w.Output() ==
        "<a x=\"&lt;&quot;&amp;&gt;&#x9;&#xA;&#xD;\"><b/>1 &lt; 2 &amp; 3 &gt; 2&#xD;\n"
        "<![CDATA[x]]]]><![CDATA[>y]]></a>");
  // What was written reads back the same.
  XmlPull p(w.Output());
  CHECK(p.Next() == Event::StartElement && p.Attributes()[0].value == "<\"&>\t\n\r");
}

// ---------- XmlDom ----------

TEST(XmlDom_ParseAndSerialize) {
  std::string xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
      "<w:doc xmlns:w=\"urn:w\"><w:t xml:space=\"preserve\">  two spaces </w:t><w:e/>"
      "<!--c--><w:t>a&#13;b</w:t><w:x a=\"tab&#9;newline&#10;\"/></w:doc>";
  XmlDom dom;
  CHECK(dom.Parse(xml).Ok());
  std::string out = dom.Serialize();
  CHECK(out.starts_with("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<w:doc"));
  CHECK(out.find("<w:t xml:space=\"preserve\">  two spaces </w:t><w:e/><!--c-->") != std::string::npos);
  CHECK(Canon(out) == Canon(xml));
  // Carriage returns and attribute whitespace survive a second round.
  XmlDom again;
  CHECK(again.Parse(out).Ok());
  CHECK(std::string(again.Root().child("w:t").next_sibling("w:t").text().get()) == "a\rb");
  CHECK(std::string(again.Root().child("w:x").attribute("a").value()) == "tab\tnewline\n");
}

TEST(XmlDom_ChecksBeforeParsing) {
  XmlDom dom;
  CHECK_EQ(dom.Parse("<!DOCTYPE a><a/>").code, Status::Code::Rejected);
  CHECK_EQ(dom.Parse("<p:a/>").code, Status::Code::Damaged);
  Limits limits;
  limits.maxDepth = 2;
  CHECK_EQ(dom.Parse("<a><b><c/></b></a>", limits).code, Status::Code::TooLarge);
}

TEST(XmlDom_Utf16DeclarationBecomesUtf8) {
  std::u16string text = u"﻿<?xml version=\"1.0\" encoding=\"UTF-16\" standalone=\"yes\"?><a>é</a>";
  XmlDom dom;
  CHECK(dom.Parse(std::string_view(reinterpret_cast<const char*>(text.data()), text.size() * 2)).Ok());
  CHECK(dom.Serialize() == "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n<a>\xC3\xA9</a>");
}

TEST(XmlDom_NamespaceHelpers) {
  XmlDom dom;
  CHECK(dom.Parse("<r xmlns=\"urn:d\" xmlns:p=\"urn:p\"><p:a p:x=\"1\" y=\"2\"><b xmlns:p=\"urn:inner\" p:z=\"3\"/></p:a></r>")
            .Ok());
  auto a = dom.Root().first_child();
  auto b = a.first_child();
  CHECK(NamespaceOf(dom.Root()) == "urn:d");
  CHECK(NamespaceOf(a) == "urn:p" && LocalNameOf(a) == "a");
  CHECK(NamespaceOf(b) == "urn:d");
  CHECK(IsElement(a, "urn:p", "a") && !IsElement(a, "urn:d", "a"));
  CHECK(FindAttribute(a, "urn:p", "x").value() == std::string("1"));
  CHECK(FindAttribute(a, "", "y").value() == std::string("2"));
  CHECK(!FindAttribute(a, "urn:d", "y"));                     // unprefixed attributes have no namespace
  CHECK(FindAttribute(b, "urn:inner", "z").value() == std::string("3"));  // the nearest declaration wins
  CHECK(ExpandedName(a) == "{urn:p}a");
  CHECK(!LookupNamespace(a, "q").has_value());
}

// ---------- Canonical form ----------

TEST(Canonical_IgnoresSpellingButNotMeaning) {
  // Prefixes, attribute order, quotes, empty-element form, CDATA and references don't matter.
  CHECK(Canon("<a:x xmlns:a=\"u\" a:p=\"1\" q='2'/>") == Canon("<b:x q=\"2\" b:p=\"1\" xmlns:b=\"u\"></b:x>"));
  CHECK(Canon("<x>a&amp;b</x>") == Canon("<x><![CDATA[a&b]]></x>"));
  CHECK(Canon("<x>&#65;</x>") == Canon("<x>A</x>"));
  CHECK(Canon("<?xml version=\"1.0\"?>\n<x>\n  <y/>\n</x>") == Canon("<x><y/></x>"));
  // Meaning does matter.
  CHECK(Canon("<a:x xmlns:a=\"u\"/>") != Canon("<a:x xmlns:a=\"v\"/>"));
  CHECK(Canon("<x>a</x>") != Canon("<x>b</x>"));
  CHECK(Canon("<x a=\"1\"/>") != Canon("<x a=\"2\"/>"));
  CHECK(Canon("<x><y/><z/></x>") != Canon("<x><z/><y/></x>"));
  CHECK(Canon("<t> </t>") != Canon("<t/>"));  // whitespace as the only content is kept
  CHECK(Canon("<x><!--c--></x>") != Canon("<x/>"));
}

TEST(Canonical_ResolvesMcPrefixLists) {
  const char* mc = "http://schemas.openxmlformats.org/markup-compatibility/2006";
  std::string a = std::string("<r xmlns:mc=\"") + mc + "\" xmlns:w14=\"urn:w14\" mc:Ignorable=\"w14\"/>";
  std::string b = std::string("<r xmlns:m=\"") + mc + "\" xmlns:x=\"urn:w14\" m:Ignorable=\"x\"/>";
  std::string c = std::string("<r xmlns:mc=\"") + mc + "\" xmlns:w14=\"urn:other\" mc:Ignorable=\"w14\"/>";
  CHECK(Canon(a) == Canon(b));
  CHECK(Canon(a) != Canon(c));
}
