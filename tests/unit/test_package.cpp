#include "TestHarness.h"

#include "PkgTestUtil.h"
#include "pkg/Canonical.h"
#include "pkg/Mce.h"
#include "pkg/Package.h"
#include "pkg/References.h"
#include "pkg/RoundTrip.h"
#include "pkg/XmlDom.h"

using namespace fw::pkg;
using test::Bytes;
using test::MakeZip;
using test::MiniPackage;
using test::OpenBytes;
using test::Text;

// ---------- Part names and targets ----------

TEST(Package_ResolveTarget) {
  CHECK(ResolveTarget("/word/document.xml", "styles.xml") == "/word/styles.xml");
  CHECK(ResolveTarget("/word/document.xml", "media/image1.png") == "/word/media/image1.png");
  CHECK(ResolveTarget("/word/document.xml", "../customXml/item1.xml") == "/customXml/item1.xml");
  CHECK(ResolveTarget("/word/document.xml", "./theme/../styles.xml") == "/word/styles.xml");
  CHECK(ResolveTarget("/word/document.xml", "/xl/x.xml") == "/xl/x.xml");
  CHECK(ResolveTarget("/word/document.xml", "../../../up.xml") == "/up.xml");  // clamped at the root
  CHECK(ResolveTarget("/", "word/document.xml") == "/word/document.xml");
  CHECK(ResolveTarget("/word/document.xml", "#_Toc1").empty());
}

TEST(Package_RelsPartNames) {
  CHECK(RelsPartFor("/") == "/_rels/.rels");
  CHECK(RelsPartFor("/word/document.xml") == "/word/_rels/document.xml.rels");
  CHECK(SourceOfRelsPart("/_rels/.rels") == "/");
  CHECK(SourceOfRelsPart("/word/_rels/document.xml.rels") == "/word/document.xml");
  CHECK(SourceOfRelsPart("/word/_RELS/Document.xml.RELS") == "/word/Document.xml");
  CHECK(SourceOfRelsPart("/word/document.xml").empty());
  CHECK(SourceOfRelsPart("/word/rels/document.xml.rels").empty());
  CHECK(PartKey("/Word/Media/A%20B.PNG") == PartKey("/word/media/a b.png"));
  CHECK(PartKey("/caf%C3%A9.png") == PartKey("/caf\xC3\xA9.png"));
}

// ---------- Opening ----------

TEST(Package_Open) {
  Status st;
  auto pkg = OpenBytes(MakeZip(MiniPackage()), st);
  CHECK(st.Ok() && pkg);
  CHECK_EQ(pkg->Parts().size(), size_t{5});  // the directory entry isn't a part
  CHECK(pkg->FindPart("/WORD/document.xml") != nullptr);
  CHECK(pkg->FindPart("/word/media/a%20b.png") != nullptr);
  CHECK(pkg->ContentTypeOf("/word/document.xml") == "application/test.main+xml");  // override, any case
  CHECK(pkg->ContentTypeOf("/word/other.xml") == "application/xml");               // default, any case
  CHECK(pkg->ContentTypeOf("/word/media/a b.png") == "image/png");
  CHECK(pkg->ContentTypeOf("/word/none.bin").empty());
  CHECK(pkg->IsXmlPart("/word/document.xml") && pkg->IsXmlPart("/_rels/.rels"));
  CHECK(!pkg->IsXmlPart("/word/media/a b.png"));

  const RelationshipSet* root = pkg->RelationshipsOf("/");
  CHECK(root && root->items.size() == 1 && root->items[0].targetPart == "/word/document.xml");
  const RelationshipSet* doc = pkg->RelationshipsOf("/word/document.xml");
  CHECK(doc && doc->items.size() == 2);
  CHECK(doc->Find("rId1") && doc->Find("rId1")->targetPart == "/word/media/a%20b.png");
  CHECK(pkg->FindPart(doc->Find("rId1")->targetPart) != nullptr);
  CHECK(doc->Find("rId2")->external && doc->Find("rId2")->targetPart.empty());
  CHECK(pkg->Verify().Ok());
}

TEST(Package_OpenRefusesBrokenPackages) {
  Status st;
  auto parts = MiniPackage();
  parts.erase(parts.begin());  // no [Content_Types].xml
  CHECK(OpenBytes(MakeZip(parts), st) == nullptr);
  CHECK_EQ(st.code, Status::Code::Damaged);
  CHECK(st.message.find("[Content_Types].xml") != std::string::npos);

  parts = MiniPackage();
  parts[4].second = "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
  CHECK(OpenBytes(MakeZip(parts), st) == nullptr);
  CHECK_EQ(st.code, Status::Code::Damaged);
  CHECK(st.message.find("word/_rels/document.xml.rels") != std::string::npos);

  parts = MiniPackage();
  parts.push_back({"WORD/DOCUMENT.XML", "<x/>"});  // OPC part names ignore case
  CHECK(OpenBytes(MakeZip(parts), st) == nullptr);
  CHECK_EQ(st.code, Status::Code::Damaged);

  parts = MiniPackage();
  parts[3].second = "<w:document xmlns:w=\"urn:w\"><!DOCTYPE x></w:document>";
  auto pkg = OpenBytes(MakeZip(parts), st);
  CHECK(pkg && st.Ok());  // only read on demand...
  CHECK_EQ(pkg->Verify().code, Status::Code::Rejected);  // ...or by Verify
}

// ---------- Saving ----------

TEST(Package_SaveWithoutEditsIsIdentical) {
  auto zip = MakeZip(MiniPackage(), ZipWriter::Compression::Deflate, "comment");
  Status st;
  auto pkg = OpenBytes(zip, st);
  MemorySink sink;
  CHECK(pkg->Save(sink).Ok());
  CHECK(sink.bytes == zip);
}

TEST(Package_SaveRewritesOnlyWhatChanged) {
  auto zip = MakeZip(MiniPackage());
  Status st;
  auto pkg = OpenBytes(zip, st);
  pkg->SetPart("/word/document.xml", Bytes("<w:document xmlns:w=\"urn:w\"/>"));
  pkg->SetPart("/word/new.xml", Bytes("<new/>"));
  pkg->DeletePart("/word/media/a b.png");
  CHECK(pkg->FindPart("/word/media/a b.png") == nullptr);
  MemorySink sink;
  CHECK(pkg->Save(sink).Ok());

  auto again = OpenBytes(sink.bytes, st);
  CHECK(st.Ok() && again);
  const auto& before = pkg->Zip().Entries();
  const auto& after = again->Zip().Entries();
  CHECK_EQ(after.size(), size_t{6});
  CHECK(after[3].name == "word/document.xml");  // in its place
  CHECK(after[5].name == "word/new.xml");       // new parts at the end
  std::vector<uint8_t> a, b;
  for (size_t i : {size_t{0}, size_t{1}, size_t{2}, size_t{4}}) {
    CHECK(before[i].name == after[i].name);
    CHECK(pkg->Zip().ReadRecord(before[i], a).Ok() && again->Zip().ReadRecord(after[i], b).Ok() && a == b);
  }
  CHECK(again->ReadPart("/word/document.xml", a).Ok() && Text(a) == "<w:document xmlns:w=\"urn:w\"/>");
  CHECK(again->ReadPart("/word/new.xml", a).Ok() && Text(a) == "<new/>");
}

TEST(Package_SetRelationshipsAndContentTypes) {
  Status st;
  auto pkg = OpenBytes(MakeZip(MiniPackage()), st);
  RelationshipSet set = *pkg->RelationshipsOf("/word/document.xml");
  set.items.push_back({"rId3", "urn:styles", "styles.xml", false, {}});
  pkg->SetRelationships(set);
  ContentTypes types = pkg->Types();
  types.overrides.emplace_back("/word/styles.xml", "application/test.styles+xml");
  pkg->SetContentTypes(types);
  pkg->SetPart("/word/styles.xml", Bytes("<styles/>"));
  MemorySink sink;
  CHECK(pkg->Save(sink).Ok());

  auto again = OpenBytes(sink.bytes, st);
  CHECK(st.Ok() && again);
  const RelationshipSet* doc = again->RelationshipsOf("/word/document.xml");
  CHECK(doc && doc->items.size() == 3 && doc->Find("rId3")->targetPart == "/word/styles.xml");
  CHECK(doc->Find("rId2")->external);
  CHECK(again->ContentTypeOf("/word/styles.xml") == "application/test.styles+xml");
  CHECK(again->ContentTypeOf("/word/document.xml") == "application/test.main+xml");
}

// ---------- Markup Compatibility ----------

static const char* kMc = "http://schemas.openxmlformats.org/markup-compatibility/2006";

static std::string McDoc(std::string_view body) {
  return std::string("<w:body xmlns:w=\"urn:w\" xmlns:mc=\"") + kMc +
         "\" xmlns:wps=\"urn:wps\" xmlns:w14=\"urn:w14\" xmlns:x=\"urn:x\" mc:Ignorable=\"w14 x\" "
         "mc:ProcessContent=\"x:wrap\">" +
         std::string(body) + "</w:body>";
}

TEST(Mce_SelectBranch) {
  XmlDom dom;
  CHECK(dom.Parse(McDoc("<mc:AlternateContent>"
                        "<mc:Choice Requires=\"undeclared\"><w:a/></mc:Choice>"
                        "<mc:Choice Requires=\"w14 x\"><w:b/></mc:Choice>"
                        "<mc:Choice Requires=\"wps\"><w:c/></mc:Choice>"
                        "<mc:Fallback><w:d/></mc:Fallback></mc:AlternateContent>"))
            .Ok());
  auto ac = dom.Root().first_child();
  auto name = [](pugi::xml_node branch) { return std::string(branch.first_child().name()); };
  CHECK(name(mce::SelectBranch(ac, mce::Understood{"urn:w", "urn:wps"})) == "w:c");
  CHECK(name(mce::SelectBranch(ac, mce::Understood{"urn:w", "urn:w14", "urn:x"})) == "w:b");
  CHECK(name(mce::SelectBranch(ac, mce::Understood{"urn:w"})) == "w:d");
  CHECK(mce::Classify(ac, {}) == mce::Handling::AlternateContent);

  mce::KeepOnlyBranch(ac, mce::SelectBranch(ac, mce::Understood{"urn:w", "urn:wps"}));
  CHECK(std::distance(ac.children().begin(), ac.children().end()) == 1);
  CHECK(name(ac.first_child()) == "w:c");
}

TEST(Mce_ClassifyAndMustUnderstand) {
  XmlDom dom;
  CHECK(dom.Parse(McDoc("<w:p/><w14:glow/><x:wrap><w:p/></x:wrap><x:other/><wps:shape/>")).Ok());
  mce::Understood understood{"urn:w"};
  auto child = dom.Root().first_child();
  CHECK(mce::Classify(child, understood) == mce::Handling::Read);
  CHECK(mce::Classify(child = child.next_sibling(), understood) == mce::Handling::Keep);
  CHECK(mce::Classify(child = child.next_sibling(), understood) == mce::Handling::ProcessContent);
  CHECK(mce::Classify(child = child.next_sibling(), understood) == mce::Handling::Keep);
  CHECK(mce::Classify(child = child.next_sibling(), understood) == mce::Handling::Unknown);  // not ignorable
  auto ignorable = mce::IgnorableNamespaces(dom.Root().first_child());
  CHECK(ignorable.size() == 2 && ignorable[0] == "urn:w14" && ignorable[1] == "urn:x");

  CHECK(mce::CheckPart(dom.Root(), understood).Editable());
  XmlDom must;
  CHECK(must.Parse(std::string("<w:r xmlns:w=\"urn:w\" xmlns:mc=\"") + kMc +
                   "\" xmlns:v=\"urn:v\"><w:x mc:MustUnderstand=\"v\"/><mc:AlternateContent/></w:r>")
            .Ok());
  auto report = mce::CheckPart(must.Root(), understood);
  CHECK(!report.Editable() && report.mustUnderstand.size() == 1 && report.mustUnderstand[0] == "urn:v");
  CHECK_EQ(report.alternateContent, 1);
  understood.Add("urn:v");
  CHECK(mce::CheckPart(must.Root(), understood).Editable());
}

// ---------- References ----------

static const char* kDocWithRefs =
    "<w:document xmlns:w=\"urn:w\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
    "xmlns:a=\"urn:a\"><w:p><w:hyperlink r:id=\"rId5\" w:id=\"rId5\"><a:blip r:embed=\"rId5\" r:link=\"\"/>"
    "</w:hyperlink><w:other r:id=\"rId6\"/></w:p></w:document>";

TEST(References_FindRelationshipIds) {
  auto registry = ReferenceRegistry::Default();
  XmlDom dom;
  CHECK(dom.Parse(kDocWithRefs).Ok());
  auto refs = registry.Find(dom.Root(), RefKind::RelationshipId);
  CHECK_EQ(refs.size(), size_t{3});  // w:id isn't one, and an empty r:link refers to nothing
  CHECK(std::string(refs[0].attr.name()) == "r:id" && std::string(refs[1].attr.name()) == "r:embed");
  CHECK(registry.Find(dom.Root(), RefKind::BookmarkPair).empty());
  CHECK(registry.ClassifyExtension("{78C0D931-6437-407d-A8EE-F0AAD7539E65}") == ExtensionClass::Unknown);
  registry.AddExtension("{78C0D931-6437-407d-A8EE-F0AAD7539E65}", ExtensionClass::Registry);
  CHECK(registry.ClassifyExtension("{78C0D931-6437-407d-A8EE-F0AAD7539E65}") == ExtensionClass::Registry);
}

TEST(References_RenameReachesKeptContent) {
  auto registry = ReferenceRegistry::Default();
  std::string hooked;
  registry.AddHook([&](RefKind, std::string_view from, std::string_view to) {
    hooked = std::string(from) + "->" + std::string(to);
  });
  XmlDom dom;
  CHECK(dom.Parse(kDocWithRefs).Ok());

  // Kept content: an unknown element holding a reference, and an interned attribute.
  RoundTripStore store;
  Fragment f;
  f.xml = RoundTripStore::SerializeFragment(dom.Root().first_child().child("w:hyperlink").child("a:blip"));
  store.Add(1, f);
  AttrSetId set = store.Intern({{"r:id", std::string(kOfficeRelationshipsNs), "rId5"}, {"w:x", "urn:w", "rId5"}});
  store.SetNodeAttributes(2, set);

  size_t changed = registry.Rename(RefKind::RelationshipId, "rId5", "rId9", dom.Root(), &store);
  CHECK_EQ(changed, size_t{4});  // two in the DOM, one in the fragment, one in the set
  CHECK(std::string(dom.Root().first_child().child("w:hyperlink").attribute("r:id").value()) == "rId9");
  CHECK(std::string(dom.Root().first_child().child("w:hyperlink").attribute("w:id").value()) == "rId5");
  CHECK(store.FragmentsOf(1, Slot::After)[0]->xml.find("r:embed=\"rId9\"") != std::string::npos);
  CHECK(store.Attributes(set)[0].value == "rId9" && store.Attributes(set)[1].value == "rId5");
  CHECK(hooked == "rId5->rId9");
}
