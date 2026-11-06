#include "TestHarness.h"

#include <string>
#include <vector>

#include "pkg/Canonical.h"
#include "pkg/RoundTrip.h"
#include "pkg/XmlDom.h"
#include "pkg/XmlWriter.h"

using namespace fw::pkg;

// A minimal document model, to exercise the round-trip store the way the real engines will.
// It knows <doc>, <para align=...>, <props> with <bold/> and <italic/>, and <run>text</run>
// in the namespace urn:test. Everything else is kept in the store.
namespace {

constexpr std::string_view kNs = "urn:test";
constexpr NodeId kDocId = 1;

struct Run {
  NodeId id = kNoNode;
  std::string text;
};

struct Para {
  NodeId id = kNoNode;
  NodeId propsId = kNoNode;  // property containers get their own id, for their unknown children
  bool hasProps = false, bold = false, italic = false;
  std::string align;
  std::vector<Run> runs;
};

struct Doc {
  AttrSetId rootAttrs = 0;
  std::vector<Para> paras;
  NodeId nextId = kDocId + 1;
  RoundTripStore store;

  NodeId NewId() { return nextId++; }
};

bool IsTest(pugi::xml_node n, std::string_view local) { return IsElement(n, kNs, local); }

void ReadPara(Doc& doc, pugi::xml_node node, Para& para) {
  para.align = node.attribute("align").value();
  doc.store.SetNodeAttributes(para.id, doc.store.CaptureAttributes(node, [](pugi::xml_attribute a) {
    return std::string_view(a.name()) == "align";
  }));
  doc.store.CaptureChildren(node, para.id, [&](pugi::xml_node child) -> NodeId {
    if (IsTest(child, "props")) {
      para.hasProps = true;
      para.propsId = doc.NewId();
      doc.store.CaptureChildren(child, para.propsId, [&](pugi::xml_node prop) -> NodeId {
        if (IsTest(prop, "bold")) return para.bold = true, kFieldNode;
        if (IsTest(prop, "italic")) return para.italic = true, kFieldNode;
        return kNoNode;
      });
      return kFieldNode;
    }
    if (IsTest(child, "run")) {
      para.runs.push_back({doc.NewId(), child.text().get()});
      return para.runs.back().id;
    }
    return kNoNode;
  }, [](pugi::xml_node n) { return std::string_view(n.name()).ends_with("rangeStart"); });
}

bool Read(std::string_view xml, Doc& doc) {
  XmlDom dom;
  if (!dom.Parse(xml).Ok() || !IsTest(dom.Root(), "doc")) return false;
  doc.rootAttrs = doc.store.CaptureAttributes(dom.Root(), {});
  // Ids are handed out as the children are seen, so the paragraphs are read in the callback.
  doc.store.CaptureChildren(dom.Root(), kDocId, [&](pugi::xml_node child) -> NodeId {
    if (!IsTest(child, "para")) return kNoNode;
    doc.paras.emplace_back();
    doc.paras.back().id = doc.NewId();
    ReadPara(doc, child, doc.paras.back());
    return doc.paras.back().id;
  });
  return true;
}

std::string Write(const Doc& doc) {
  const RoundTripStore& s = doc.store;
  XmlWriter w;
  w.Declaration();
  w.StartElement("doc");
  s.WriteAttributes(w, doc.rootAttrs);
  s.WriteFragmentsAfterChild(w, kDocId, "");
  for (const auto& p : doc.paras) {
    s.WriteFragments(w, p.id, Slot::Before);
    w.StartElement("para");
    if (!p.align.empty()) w.Attribute("align", p.align);
    s.WriteAttributes(w, s.NodeAttributes(p.id));
    s.WriteFragmentsAfterChild(w, p.id, "");
    if (p.hasProps) {
      w.StartElement("props");
      s.WriteFragmentsAfterChild(w, p.propsId, "");
      if (p.bold) {
        w.StartElement("bold");
        w.EndElement();
        s.WriteFragmentsAfterChild(w, p.propsId, "{urn:test}bold");
      }
      if (p.italic) {
        w.StartElement("italic");
        w.EndElement();
        s.WriteFragmentsAfterChild(w, p.propsId, "{urn:test}italic");
      }
      s.WriteFragments(w, p.propsId, Slot::End);
      w.EndElement();
      s.WriteFragmentsAfterChild(w, p.id, "{urn:test}props");
    }
    for (const auto& r : p.runs) {
      s.WriteFragments(w, r.id, Slot::Before);
      w.StartElement("run");
      w.Text(r.text);
      w.EndElement();
      s.WriteFragments(w, r.id, Slot::After);
    }
    s.WriteFragments(w, p.id, Slot::End);
    w.EndElement();
    s.WriteFragments(w, p.id, Slot::After);
  }
  s.WriteFragments(w, kDocId, Slot::End);
  w.EndElement();
  return w.Take();
}

std::string Canon(std::string_view xml) {
  std::string out;
  Status st = CanonicalizeXml(xml, out);
  return st.Ok() ? out : "ERROR: " + st.message;
}

const char* kSample =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<doc xmlns=\"urn:test\" xmlns:x=\"urn:unknown\" xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\" "
    "mc:Ignorable=\"x\" x:docAttr=\"1\">\n"
    "  <x:before/>\n"
    "  <para align=\"left\" x:id=\"p1\">\n"
    "    <props><bold/><x:glow x:radius=\"5\"/><italic/></props>\n"
    "    <x:afterProps/>\n"
    "    <run>Hello </run>\n"
    "    <x:inline a=\"1\"><x:deep>text</x:deep></x:inline>\n"
    "    <run>world</run>\n"
    "    <!-- a comment -->\n"
    "  </para>\n"
    "  <x:between/>\n"
    "  <para><x:rangeStart x:id=\"7\"/><run>Second</run><x:tail/></para>\n"
    "  <x:end/>\n"
    "</doc>\n";

}  // namespace

TEST(RoundTrip_ReadWriteKeepsEverything) {
  Doc doc;
  CHECK(Read(kSample, doc));
  CHECK_EQ(doc.paras.size(), size_t{2});
  CHECK(doc.paras[0].bold && doc.paras[0].italic && doc.paras[0].runs.size() == 2);
  CHECK_EQ(doc.store.FragmentCount(), size_t{9});
  std::string out = Write(doc);
  CHECK(Canon(out) == Canon(kSample));
}

TEST(RoundTrip_FragmentsAttachToNearestKnownNeighbor) {
  Doc doc;
  CHECK(Read(kSample, doc));
  const Para& p1 = doc.paras[0];
  const Para& p2 = doc.paras[1];
  CHECK_EQ(doc.store.FragmentsOf(p1.id, Slot::Before).size(), size_t{1});           // x:before: no known before it
  CHECK_EQ(doc.store.FragmentsAfterChild(p1.propsId, "{urn:test}bold").size(), size_t{1});  // x:glow
  CHECK_EQ(doc.store.FragmentsAfterChild(p1.id, "{urn:test}props").size(), size_t{1});      // x:afterProps
  CHECK_EQ(doc.store.FragmentsOf(p1.runs[0].id, Slot::After).size(), size_t{1});    // x:inline
  CHECK_EQ(doc.store.FragmentsOf(p1.runs[1].id, Slot::After).size(), size_t{1});    // the comment
  CHECK_EQ(doc.store.FragmentsOf(p1.id, Slot::After).size(), size_t{1});            // x:between
  CHECK_EQ(doc.store.FragmentsOf(p2.id, Slot::After).size(), size_t{1});            // x:end
  CHECK(doc.store.FragmentsOf(p2.runs[0].id, Slot::Before)[0]->pairEnd);           // x:rangeStart
  // Each fragment stands alone: it declares the prefixes it uses.
  const Fragment* inl = doc.store.FragmentsOf(p1.runs[0].id, Slot::After)[0];
  CHECK(inl->xml == "<x:inline xmlns:x=\"urn:unknown\" a=\"1\"><x:deep>text</x:deep></x:inline>");
  XmlDom alone;
  CHECK(alone.Parse(inl->xml).Ok());
  // Unknown attributes, namespace declarations included, are kept as sets.
  const auto& rootAttrs = doc.store.Attributes(doc.rootAttrs);
  CHECK_EQ(rootAttrs.size(), size_t{5});
  CHECK(doc.store.Attributes(doc.store.NodeAttributes(p1.id)).at(0).qname == "x:id");
}

TEST(RoundTrip_RepeatedAttributesAreInterned) {
  RoundTripStore store;
  std::vector<XmlAttr> attrs{{"x14ac:dyDescent", "urn:x14ac", "0.25"}};
  AttrSetId first = store.Intern(attrs);
  for (NodeId row = 1; row <= 10000; ++row) store.SetNodeAttributes(row, store.Intern(attrs));
  CHECK_EQ(store.AttrSetCount(), size_t{1});
  CHECK(store.NodeAttributes(9999) == first);
  CHECK(store.Intern({}) == 0 && store.Attributes(0).empty());
  CHECK(store.Intern({{"x14ac:dyDescent", "urn:x14ac", "0.3"}}) != first);
  size_t bytes = store.BytesUsed();
  CHECK(bytes > 0 && bytes < 1000);  // two small sets, not ten thousand
}

TEST(RoundTrip_SplitKeepsFragmentsWithFirstHalf) {
  Doc doc;
  CHECK(Read(kSample, doc));
  // Split the first paragraph between its runs, as pressing Enter there would.
  Para second;
  second.id = doc.NewId();
  second.runs.push_back(doc.paras[0].runs[1]);
  doc.paras[0].runs.pop_back();
  doc.store.Split(doc.paras[0].id, second.id, {second.runs[0].id});
  doc.paras.insert(doc.paras.begin() + 1, second);

  CHECK_EQ(doc.store.FragmentsOf(doc.paras[0].id, Slot::Before).size(), size_t{1});
  CHECK(doc.store.FragmentsOf(doc.paras[0].id, Slot::After).empty());
  CHECK_EQ(doc.store.FragmentsOf(second.id, Slot::After).size(), size_t{1});  // x:between still follows the text
  std::string out = Write(doc);
  CHECK(out.find("<run>Hello </run><x:inline") != std::string::npos);
  CHECK(out.find("</para><para><run>world</run><!-- a comment --></para><x:between") != std::string::npos);
  CHECK(out.find("x:id=\"p1\"") < out.find("<run>Hello"));  // attributes stay with the first half
}

TEST(RoundTrip_SplitMovesWhatFollowsTheSplitPoint) {
  // Inline content after the last run (End, as a merge leaves it) follows the split point.
  auto split = [](SplitPoint at, bool moveRun) {
    Doc doc;
    CHECK(Read(kSample, doc));
    Para& first = doc.paras[0];
    doc.store.Add(first.id, {Slot::End, {}, "<x:rangeEnd xmlns:x=\"urn:unknown\"/>", true});
    Para second;
    second.id = doc.NewId();
    if (moveRun) {
      second.runs.push_back(first.runs[1]);
      first.runs.pop_back();
    }
    doc.store.Split(first.id, second.id, at);
    doc.paras.insert(doc.paras.begin() + 1, second);
    return Write(doc);
  };
  Doc probe;
  CHECK(Read(kSample, probe));
  NodeId world = probe.paras[0].runs[1].id;

  // Between the runs: the range end and the comment follow "world" into the second half.
  std::string out = split({world}, true);
  CHECK(out.find("<run>Hello </run><x:inline") != std::string::npos);
  CHECK(out.find("</para><para><run>world</run><!-- a comment --><x:rangeEnd") != std::string::npos);
  CHECK(out.find("<x:rangeEnd xmlns:x=\"urn:unknown\"/></para><x:between") != std::string::npos);

  // At the end (Enter after the last run): nothing follows the split point but the end tag.
  out = split({}, false);
  CHECK(out.find("<!-- a comment --><x:rangeEnd xmlns:x=\"urn:unknown\"/></para><para/><x:between") != std::string::npos);

  // Inside text the model keeps itself: no child moves, but what follows the text does.
  RoundTripStore s;
  s.Add(1, {Slot::Before, {}, "<a/>"});
  s.Add(1, {Slot::Inside, "{urn:test}props", "<b/>"});
  s.Add(1, {Slot::End, {}, "<c/>"});
  s.Add(1, {Slot::After, {}, "<d/>"});
  s.Split(1, 2, {kNoNode, false});
  CHECK(s.FragmentsOf(1, Slot::Before).size() == 1 && s.FragmentsAfterChild(1, "{urn:test}props").size() == 1);
  CHECK(s.FragmentsOf(1, Slot::End).empty() && s.FragmentsOf(1, Slot::After).empty());
  CHECK(s.FragmentsOf(2, Slot::End).size() == 1 && s.FragmentsOf(2, Slot::End)[0]->xml == "<c/>");
  CHECK(s.FragmentsOf(2, Slot::After).size() == 1 && s.FragmentsOf(2, Slot::After)[0]->xml == "<d/>");

  // Splitting and merging back restores the order.
  s.Merge(1, 2);
  auto end = s.FragmentsOf(1, Slot::End);
  CHECK(end.size() == 1 && end[0]->xml == "<c/>" && s.FragmentsOf(1, Slot::After).size() == 1);
}

TEST(RoundTrip_MergeKeepsOrder) {
  Doc doc;
  CHECK(Read(kSample, doc));
  Para& first = doc.paras[0];
  Para second = doc.paras[1];
  NodeId firstMoved = second.runs[0].id;
  for (const auto& r : second.runs) first.runs.push_back(r);
  doc.store.Merge(first.id, second.id, firstMoved);
  doc.paras.pop_back();

  std::string out = Write(doc);
  // Second's inline content (the range start) joins the paragraph before its first run;
  // what sat between and after the paragraphs follows the merged one, in order.
  size_t world = out.find("<run>world</run>");
  size_t range = out.find("<x:rangeStart");
  size_t secondRun = out.find("<run>Second</run>");
  size_t tail = out.find("<x:tail");
  size_t between = out.find("<x:between");
  size_t end = out.find("<x:end");
  CHECK(world < range && range < secondRun && secondRun < tail);
  CHECK(tail < out.find("</para>") && out.find("</para>") < between && between < end);
  CHECK(Canon(out).find("ERROR") == std::string::npos);
}

TEST(RoundTrip_MergeWithoutMovedChildren) {
  RoundTripStore s;
  Fragment a{Slot::End, {}, "<a/>"};
  Fragment b{Slot::Inside, "{urn:test}props", "<b/>"};
  Fragment c{Slot::After, {}, "<c/>"};
  Fragment d{Slot::After, {}, "<d/>"};
  s.Add(1, a);
  s.Add(1, c);
  s.Add(2, b);
  s.Add(2, d);
  s.Merge(1, 2);
  auto end = s.FragmentsOf(1, Slot::End);
  auto after = s.FragmentsOf(1, Slot::After);
  CHECK(end.size() == 2 && end[0]->xml == "<a/>" && end[1]->xml == "<b/>" && end[1]->afterChild.empty());
  CHECK(after.size() == 2 && after[0]->xml == "<c/>" && after[1]->xml == "<d/>");
  CHECK(s.FragmentsOf(2, Slot::After).empty());
}

TEST(RoundTrip_DeleteKeepsPairEnds) {
  Doc doc;
  CHECK(Read(kSample, doc));
  Para& p2 = doc.paras[1];
  NodeId run = p2.runs[0].id;
  // Deleting the run takes its fragments, except the range start, which moves on.
  auto orphans = doc.store.Delete(run);
  CHECK_EQ(orphans.size(), size_t{1});
  CHECK(orphans[0].xml.find("rangeStart") != std::string::npos);
  doc.store.Add(p2.id, orphans[0]);

  RoundTripStore s;
  s.Add(5, {Slot::After, {}, "<x:plain/>"});
  s.Add(5, {Slot::Before, {}, "<x:rangeEnd/>", true});
  s.SetNodeAttributes(5, s.Intern({{"x:a", "urn:x", "1"}}));
  CHECK(s.Delete(5, 6, Slot::After).empty());
  CHECK(s.FragmentsOf(5, Slot::Before).empty() && s.NodeAttributes(5) == 0);
  CHECK(s.FragmentsOf(6, Slot::After).size() == 1 && s.FragmentsOf(6, Slot::After)[0]->pairEnd);
}

TEST(RoundTrip_Budget) {
  RoundTripStore s;
  s.SetBudget(200);
  CHECK(!s.OverBudget());
  s.Add(1, {Slot::After, {}, std::string(300, 'x')});
  CHECK(s.OverBudget());
  s.Delete(1);
  CHECK(!s.OverBudget());
}
