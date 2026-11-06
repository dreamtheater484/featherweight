// The torture test's rule for drawing objects where Word refuses the file (tools/fwtool
// DrawingPlaces.h): a shape or a floating drawing in a footnote, an endnote, a comment or a text
// box's text. Inline pictures there are fine, and a shape and its VML copy count once.
#include <map>
#include <string>

#include "DrawingPlaces.h"
#include "TestHarness.h"

using fwtool::torture::MisplacedDrawings;

namespace {

const std::string kNs =
    " xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
    " xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\""
    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\""
    " xmlns:wps=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\""
    " xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\""
    " xmlns:v=\"urn:schemas-microsoft-com:vml\"";

// A floating shape with text, as Shapes::Insert writes a callout: a DrawingML choice and,
// with `fallback`, a VML copy whose text box holds `inside` too.
std::string Shape(const std::string& inside = "<w:p/>", bool fallback = false) {
  std::string s =
      "<w:r><mc:AlternateContent><mc:Choice Requires=\"wps\"><w:drawing><wp:anchor><wp:extent cx=\"1\" cy=\"1\"/>"
      "<wp:docPr id=\"1\" name=\"Speech Bubble: Rectangle 1\"/><a:graphic><a:graphicData "
      "uri=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\"><wps:wsp><wps:spPr/><wps:txbx>"
      "<w:txbxContent>" + inside + "</w:txbxContent></wps:txbx><wps:bodyPr/></wps:wsp></a:graphicData></a:graphic>"
      "</wp:anchor></w:drawing></mc:Choice>";
  if (fallback)
    s += "<mc:Fallback><w:pict><v:shape style=\"position:absolute;width:10pt\"><v:textbox><w:txbxContent>" + inside +
         "</w:txbxContent></v:textbox></v:shape></w:pict></mc:Fallback>";
  return s + "</mc:AlternateContent></w:r>";
}

// An inline picture: allowed anywhere.
const std::string kPicture =
    "<w:r><w:drawing><wp:inline><wp:extent cx=\"1\" cy=\"1\"/><wp:docPr id=\"2\" name=\"Picture 2\"/><a:graphic>"
    "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:pic/></a:graphicData>"
    "</a:graphic></wp:inline></w:drawing></w:r>";

}  // namespace

TEST(DrawingPlaces_AShapeInAFootnote) {
  // D5 corpus pass (wipo-mm2): a callout at the start of a footnote; Word refuses the file.
  const std::string xml = "<w:footnotes" + kNs + "><w:footnote w:id=\"1\"><w:p>" + kPicture +
                          "</w:p></w:footnote><w:footnote w:id=\"24\"><w:p>" + Shape() + "<w:r><w:t>x</w:t></w:r></w:p>" +
                          "</w:footnote></w:footnotes>";
  const std::map<std::string, int> found = MisplacedDrawings("/word/footnotes.xml", xml);
  CHECK_EQ(found.size(), 1u);
  CHECK_EQ(found.count("a shape or floating drawing in a footnote (/word/footnotes.xml)"), 1u);
  CHECK(found.begin()->second == 1);
}

TEST(DrawingPlaces_ADrawingInATextBox) {
  // A text box in the body is fine, and so is a picture in its text; a shape in its text isn't,
  // counted once though the VML copy has it too.
  const std::string body = "<w:document" + kNs + "><w:body><w:p>" + Shape("<w:p>" + kPicture + "</w:p>", true) +
                           "</w:p></w:body></w:document>";
  CHECK(MisplacedDrawings("/word/document.xml", body).empty());
  const std::string nested = "<w:document" + kNs + "><w:body><w:p>" + Shape("<w:p>" + Shape() + "</w:p>", true) +
                             "</w:p></w:body></w:document>";
  const std::map<std::string, int> found = MisplacedDrawings("/word/document.xml", nested);
  CHECK_EQ(found.size(), 1u);
  CHECK(found.count("a shape or floating drawing in a text box (/word/document.xml)") == 1 && found.begin()->second == 1);
}

TEST(DrawingPlaces_VmlInACommentAndAShapeInAnEndnote) {
  const std::string comments = "<w:comments" + kNs + "><w:comment w:id=\"0\"><w:p>" + kPicture +
                               "</w:p><w:p><w:r><w:pict><v:shape style=\"position: absolute\"/></w:pict></w:r></w:p>"
                               "</w:comment></w:comments>";
  const std::map<std::string, int> inComment = MisplacedDrawings("/word/comments.xml", comments);
  CHECK(inComment.size() == 1 && inComment.count("a shape or floating drawing in a comment (/word/comments.xml)") == 1);
  // An inline shape (wps) is a drawing object too.
  const std::string endnotes =
      "<w:endnotes" + kNs + "><w:endnote w:id=\"1\"><w:p><w:r><w:drawing><wp:inline><a:graphic><a:graphicData "
      "uri=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\"><wps:wsp/></a:graphicData></a:graphic>"
      "</wp:inline></w:drawing></w:r></w:p></w:endnote></w:endnotes>";
  const std::map<std::string, int> inEndnote = MisplacedDrawings("/word/endnotes.xml", endnotes);
  CHECK(inEndnote.size() == 1 && inEndnote.count("a shape or floating drawing in an endnote (/word/endnotes.xml)") == 1);
  // A shape in the body isn't in any such place.
  CHECK(MisplacedDrawings("/word/document.xml", "<w:document" + kNs + "><w:body><w:p>" + Shape() + "</w:p></w:body></w:document>").empty());
}
