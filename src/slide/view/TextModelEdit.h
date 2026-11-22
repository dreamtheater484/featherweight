#pragma once

#include "slide/edit/Edit.h"
#include "slide/view/TextModel.h"

// TextModel over p2-model's edit layer (src/slide/edit): text editing's one door into the model.
// The tab makes one per editor and passes it to TextEditing::SetModel; tools and tests use it to
// type through TextSession as the tab does.
namespace fw::slide::view {

class EditTextModel : public TextModel {
 public:
  EditTextModel(Presentation& deck, edit::Editor& editor) : deck_(deck), editor_(editor) {}

  const SlideBase* Part(const TextTarget& t) override;
  pugi::xml_node Body(const TextTarget& t) override;
  std::unique_ptr<TextBody> Resolve(const TextTarget& t, pugi::xml_node txBody) override;
  bool SetText(const TextTarget& t, pugi::xml_node txBody, std::u16string_view step, bool coalesce, std::wstring& error) override;
  void BeginGroup(std::u16string_view step) override { editor_.BeginGroup(std::u16string(step)); }
  void EndGroup() override { editor_.EndGroup(); }
  bool SetShapeHeight(const TextTarget& t, float height) override;
  std::string AddLink(const TextTarget& t, const std::wstring& url, int slide) override;
  Presentation* Deck() override { return &deck_; }

 private:
  const Shape* ShapeOf(const TextTarget& t);
  Presentation& deck_;
  edit::Editor& editor_;
};

}  // namespace fw::slide::view
