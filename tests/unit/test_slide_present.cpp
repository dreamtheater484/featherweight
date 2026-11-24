// P3's presenting piece (p3-present): Insert > Video and Audio (src/slide/edit/MediaOps.cpp), as
// PowerPoint 2021 writes them.
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "PkgTestUtil.h"
#include "TestHarness.h"
#include "pkg/Package.h"
#include "pkg/Snapshot.h"
#include "pkg/XmlDom.h"
#include "slide/Presentation.h"
#include "slide/edit/Edit.h"
#include "slide/edit/Save.h"
#include "slide/show/Timing.h"

using namespace fw::slide;
using namespace fw::slide::edit;

namespace {

std::filesystem::path Generated() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "office" / "generated";
}

std::unique_ptr<Presentation> OpenBytes(std::vector<uint8_t> bytes) {
  fw::pkg::Status st;
  auto pk = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(std::move(bytes)), fw::pkg::Limits::Default(), st);
  return pk ? Read(std::move(pk), st) : nullptr;
}

std::unique_ptr<Presentation> Open(const char* name) {
  fw::pkg::Status st;
  auto snap = fw::pkg::Snapshot::Open((Generated() / name).wstring(), {}, st);
  if (!snap) return nullptr;
  auto span = snap->Bytes();
  return OpenBytes(std::vector<uint8_t>(span.begin(), span.end()));
}

std::string PartText(const Editor& ed, std::string_view part) {
  std::vector<uint8_t> b;
  ed.ReadPart(part, b);
  return std::string(b.begin(), b.end());
}

size_t Count(const std::string& s, std::string_view what) {
  size_t n = 0;
  for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + 1)) ++n;
  return n;
}

// A 1x1 PNG.
std::vector<uint8_t> Png() {
  return {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00,
          0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00,
          0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01,
          0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
}

// The op stores files as they are: these only need to be some bytes.
std::vector<uint8_t> Mp4() { return {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'm', 'p', '4', '2', 0, 0, 0, 0, 'i', 's', 'o', 'm'}; }
std::vector<uint8_t> Wav() { return {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E'}; }

const Shape* ShapeById(Presentation& deck, size_t slide, uint32_t id) {
  for (const Shape& sh : deck.GetSlide(slide)->shapes)
    if (sh.id == id) return &sh;
  return nullptr;
}

fw::slide::show::Timing SlideTiming(const std::string& xml) {
  static fw::pkg::XmlDom dom;  // the timing's nodes live in it
  CHECK(dom.Parse(xml).Ok());
  for (pugi::xml_node c = dom.Root().first_child(); c; c = c.next_sibling())
    if (fw::pkg::LocalNameOf(c) == "timing") return fw::slide::show::ParseTiming(c);
  return {};
}

}  // namespace

TEST(SlideInsertMedia) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  if (!deck) return;
  Editor ed(*deck);
  const std::string part = ed.SlidePart(0);

  // A video, as PowerPoint inserts one: the shape, its two relationships to the file, the poster.
  uint32_t video = 0;
  Result r = ed.InsertMedia(0, {true, Mp4(), "MP4", Png(), L"clip & co.mp4", 4000}, {100, 100, 420, 280}, &video);
  CHECK(r.ok);
  CHECK(video != 0);
  const Shape* sh = ShapeById(*deck, 0, video);
  CHECK(sh && sh->media && sh->kind == Shape::Kind::Picture);
  std::string xml = PartText(ed, part);
  CHECK(xml.find("name=\"clip &amp; co.mp4\"") != std::string::npos);
  CHECK(xml.find("action=\"ppaction://media\"") != std::string::npos);
  CHECK(xml.find("<a:videoFile r:link=") != std::string::npos);
  CHECK(xml.find("<p14:media") != std::string::npos);
  CHECK(xml.find("cmd=\"playFrom(0.0)\"") != std::string::npos);
  CHECK(xml.find("dur=\"4000\"") != std::string::npos);
  CHECK(xml.find("cmd=\"togglePause\"") != std::string::npos);
  CHECK(Count(xml, "<p:video>") == 1);
  CHECK(xml.find("xmlns:p=", xml.find("<p:timing")) == std::string::npos);  // the slide's own declaration serves

  // The show reads it: one click step (the play), the click on the video a trigger, nothing hidden.
  fw::slide::show::Timing t = SlideTiming(xml);
  CHECK(t.main.steps.size() == 1);
  CHECK(t.triggers.size() == 1 && t.triggers[0].triggerSpid == video);
  CHECK(t.initiallyHidden.empty());

  // Playback (P3): the click plays the video from its start, a click on the video pauses or plays it;
  // nothing is posed or hidden, and the step takes no time (the media plays on its own).
  {
    using fw::slide::show::Behavior;
    using fw::slide::show::MediaCommand;
    using fw::slide::show::MediaEvent;
    CHECK(t.main.steps.size() == 1 && t.main.steps[0].behaviors.size() == 1);
    const Behavior& play = t.main.steps[0].behaviors[0];
    CHECK(play.kind == Behavior::Command && play.command == MediaCommand::Play && play.commandFromMs == 0);
    CHECK(play.target.spid == video && t.main.steps[0].length == 0);
    CHECK(t.triggers[0].steps.size() == 1 && t.triggers[0].steps[0].behaviors.size() == 1);
    CHECK(t.triggers[0].steps[0].behaviors[0].command == MediaCommand::TogglePause);
    CHECK(t.targets.empty() && t.unsupportedEffects == 0);
    CHECK(t.media.size() == 1 && t.media[0].spid == video && t.media[0].video);
    CHECK(t.media[0].volume > 0.79f && t.media[0].volume < 0.81f && !t.media[0].mute && !t.media[0].loop);
    CHECK(t.media[0].showWhenStopped && !t.media[0].fullScreen && t.media[0].slides == 1);

    fw::slide::show::Animator a(&t, nullptr, nullptr);
    a.Start(0);
    CHECK(a.TakeMediaEvents().empty());
    CHECK(a.Click(10));
    std::vector<MediaEvent> ev = a.TakeMediaEvents();
    CHECK(ev.size() == 1 && ev[0].spid == video && ev[0].command == MediaCommand::Play && ev[0].fromMs == 0);
    CHECK(!a.Running(10) && !a.HasNextStep());
    CHECK(a.PoseOf({video}).visible);
    for (int k = 0; k < 3; ++k) {  // every click on it
      CHECK(a.Trigger(video, 20 + k));
      ev = a.TakeMediaEvents();
      CHECK(ev.size() == 1 && ev[0].command == MediaCommand::TogglePause);
    }
    CHECK(a.TakeMediaEvents().empty());
    a.FinishAll(40);  // coming back to the slide: built, nothing plays
    CHECK(a.TakeMediaEvents().empty());
  }

  // An audio after it: its own node, which stops when the slide changes; the same video again shares
  // the file.
  uint32_t audio = 0, again = 0;
  CHECK(ed.InsertMedia(0, {false, Wav(), ".wav", Png(), L"", 1500}, {500, 300, 560, 360}, &audio).ok);
  CHECK(ed.InsertMedia(0, {true, Mp4(), "mp4", Png(), L"clip.mp4", 4000}, {100, 300, 420, 480}, &again).ok);
  xml = PartText(ed, part);
  CHECK(xml.find("name=\"Audio ") != std::string::npos);
  CHECK(Count(xml, "<p:audio>") == 1 && Count(xml, "<p:video>") == 2);
  CHECK(xml.find("evt=\"onStopAudio\"") != std::string::npos);
  CHECK(Count(xml, "cmd=\"playFrom(0.0)\"") == 3);
  t = SlideTiming(xml);
  CHECK(t.main.steps.size() == 3);
  CHECK(t.triggers.size() == 3);
  std::vector<uint8_t> have;
  CHECK(ed.ReadPart("/ppt/media/media1.mp4", have) && have == Mp4());
  CHECK(!ed.ReadPart("/ppt/media/media3.mp4", have));

  // Refused, changing nothing: a kind PowerPoint doesn't insert, an empty file, a poster that isn't
  // PNG or JPEG, a slide that isn't there.
  const std::string before = PartText(ed, part);
  CHECK(!ed.InsertMedia(0, {true, Mp4(), "exe", Png(), L"", 0}, {0, 0, 10, 10}).ok);
  CHECK(!ed.InsertMedia(0, {true, {}, "mp4", Png(), L"", 0}, {0, 0, 10, 10}).ok);
  CHECK(!ed.InsertMedia(0, {true, Mp4(), "mp4", Wav(), L"", 0}, {0, 0, 10, 10}).ok);
  CHECK(!ed.InsertMedia(99, {true, Mp4(), "mp4", Png(), L"", 0}, {0, 0, 10, 10}).ok);
  CHECK(PartText(ed, part) == before);

  // Saved and opened again: the shapes are media, the files are there.
  std::vector<uint8_t> saved;
  CHECK(SaveToBytes(ed, saved).Ok());
  auto back = OpenBytes(saved);
  CHECK(back != nullptr);
  if (!back) return;
  CHECK(ShapeById(*back, 0, video) && ShapeById(*back, 0, video)->media);
  CHECK(ShapeById(*back, 0, audio) && ShapeById(*back, 0, audio)->media);
  Editor ed2(*back);
  CHECK(ed2.ReadPart("/ppt/media/media1.mp4", have) && have == Mp4());
  CHECK(ed2.ReadPart("/ppt/media/media1.wav", have) && have == Wav());
  // As PowerPoint writes them: a video (or audio) and a media relationship to the same file; the
  // content types by extension.
  {
    fw::pkg::Status st;
    auto pk = fw::pkg::Package::Open(fw::pkg::Snapshot::FromBytes(saved), fw::pkg::Limits::Default(), st);
    CHECK(pk != nullptr);
    if (!pk) return;
    const fw::pkg::RelationshipSet* rs = pk->RelationshipsOf(part);
    CHECK(rs != nullptr);
    auto rel = [&](std::string_view type, std::string_view target) {
      int n = 0;
      for (const fw::pkg::Relationship& x : rs ? rs->items : std::vector<fw::pkg::Relationship>{})
        if (x.type.ends_with(type) && x.targetPart == target && !x.external) ++n;
      return n;
    };
    CHECK(rel("/relationships/video", "/ppt/media/media1.mp4") == 2);  // one per video shape
    CHECK(rel("/2007/relationships/media", "/ppt/media/media1.mp4") == 2);
    CHECK(rel("/relationships/audio", "/ppt/media/media1.wav") == 1);
    CHECK(rel("/2007/relationships/media", "/ppt/media/media1.wav") == 1);
    CHECK(pk->ContentTypeOf("/ppt/media/media1.mp4") == "video/mp4");
    CHECK(pk->ContentTypeOf("/ppt/media/media1.wav") == "audio/wav");
  }

  // Deleting the videos takes their timing with them and leaves the audio's; on save the video file
  // goes, the sound stays.
  CHECK(ed.DeleteShapes(0, {video, again}).ok);
  xml = PartText(ed, part);
  CHECK(Count(xml, "<p:video>") == 0 && Count(xml, "<p:audio>") == 1);
  CHECK(Count(xml, "cmd=\"playFrom(0.0)\"") == 1);
  CHECK(Count(xml, "spid=\"" + std::to_string(video) + "\"") == 0);
  CHECK(SaveToBytes(ed, saved).Ok());
  back = OpenBytes(saved);
  CHECK(back != nullptr);
  if (!back) return;
  Editor ed3(*back);
  CHECK(!ed3.ReadPart("/ppt/media/media1.mp4", have));
  CHECK(ed3.ReadPart("/ppt/media/media1.wav", have));

  // Undo brings them back.
  CHECK(ed.Undo(1).ok);
  CHECK(Count(PartText(ed, part), "<p:video>") == 2);
}

TEST(SlideMediaPlayback) {
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  if (!deck) return;
  Editor ed(*deck);
  const std::string part = ed.SlidePart(0);
  uint32_t video = 0, audio = 0;
  CHECK(ed.InsertMedia(0, {true, Mp4(), "mp4", Png(), L"clip.mp4", 4000}, {100, 100, 420, 280}, &video).ok);
  CHECK(ed.InsertMedia(0, {false, Wav(), "wav", Png(), L"", 1500}, {500, 300, 560, 360}, &audio).ok);

  // As inserted: 80 %, nothing else.
  fw::slide::edit::MediaPlayback p;
  bool isVideo = false;
  CHECK(ed.GetMediaPlayback(0, video, &p, &isVideo) && isVideo);
  CHECK(p.volume > 0.79 && p.volume < 0.81 && !p.mute && !p.loop && !p.hideWhenStopped && !p.fullScreen && !p.acrossSlides);
  CHECK(ed.GetMediaPlayback(0, audio, &p, &isVideo) && !isVideo);
  CHECK(!ed.GetMediaPlayback(0, 2, &p));  // the title: no media node

  // The video: low, muted, looping, hidden while stopped, full screen; across slides is the audio's only.
  const std::string before = PartText(ed, part);
  fw::slide::edit::MediaPlayback v;
  v.volume = 0.25;
  v.mute = v.loop = v.hideWhenStopped = v.fullScreen = v.acrossSlides = true;
  CHECK(ed.SetMediaPlayback(0, video, v).ok);
  std::string xml = PartText(ed, part);
  CHECK(xml.find("<p:video fullScrn=\"1\"><p:cMediaNode vol=\"25000\" mute=\"1\" showWhenStopped=\"0\">") != std::string::npos);
  CHECK(xml.find("repeatCount=\"indefinite\"") != std::string::npos);
  CHECK(xml.find("numSld") == std::string::npos);
  fw::slide::show::Timing t = SlideTiming(xml);
  CHECK(t.media.size() == 2);
  if (t.media.size() == 2) {
    CHECK(t.media[0].spid == video && t.media[0].loop && t.media[0].mute && !t.media[0].showWhenStopped);
    CHECK(t.media[0].fullScreen && t.media[0].volume > 0.24f && t.media[0].volume < 0.26f && t.media[0].slides == 1);
  }
  CHECK(ed.GetMediaPlayback(0, video, &p) && p.mute && p.loop && p.hideWhenStopped && p.fullScreen && !p.acrossSlides);
  // Hidden while not playing: the show starts the poster hidden.
  bool waits = false;
  for (const fw::slide::show::TargetKey& k : t.initiallyHidden) waits |= k.spid == video;
  CHECK(waits);

  // The audio across slides; back to the defaults leaves only vol.
  fw::slide::edit::MediaPlayback a;
  a.acrossSlides = true;
  CHECK(ed.SetMediaPlayback(0, audio, a).ok);
  CHECK(PartText(ed, part).find("numSld=\"999\"") != std::string::npos);
  CHECK(ed.SetMediaPlayback(0, video, {}).ok);
  xml = PartText(ed, part);
  CHECK(xml.find("<p:video><p:cMediaNode vol=\"80000\"><p:cTn") != std::string::npos);
  CHECK(xml.find("repeatCount") == std::string::npos);

  // Refused for what isn't media, changing nothing; undone step by step.
  const std::string now = PartText(ed, part);
  CHECK(!ed.SetMediaPlayback(0, 2, v).ok);
  CHECK(!ed.SetMediaPlayback(9, video, v).ok);
  CHECK(PartText(ed, part) == now);
  CHECK(ed.Undo(3).ok);
  CHECK(PartText(ed, part) == before);
}

TEST(SlideMediaStart) {
  using fw::slide::edit::MediaStart;
  using fw::slide::show::MediaCommand;
  auto deck = Open("simple.pptx");
  CHECK(deck != nullptr);
  if (!deck) return;
  Editor ed(*deck);
  const std::string part = ed.SlidePart(0);
  uint32_t video = 0;
  CHECK(ed.InsertMedia(0, {true, Mp4(), "mp4", Png(), L"clip.mp4", 4000}, {100, 100, 420, 280}, &video).ok);
  const std::string before = PartText(ed, part);
  MediaStart s = MediaStart::WhenClickedOn;
  CHECK(ed.GetMediaStart(0, video, &s) && s == MediaStart::InClickSequence);
  CHECK(!ed.GetMediaStart(0, 2, &s));

  // Automatically: the play step after the previous (the slide's start here), so the show plays it at once.
  CHECK(ed.SetMediaStart(0, video, MediaStart::Automatically).ok);
  CHECK(ed.GetMediaStart(0, video, &s) && s == MediaStart::Automatically);
  fw::slide::show::Timing t = SlideTiming(PartText(ed, part));
  CHECK(t.main.steps.size() == 1 && t.main.steps[0].start != fw::slide::show::StepStart::Click);
  {
    fw::slide::show::Animator a(&t, nullptr, nullptr);
    a.Start(0);
    std::vector<fw::slide::show::MediaEvent> ev = a.TakeMediaEvents();
    CHECK(ev.size() == 1 && ev[0].command == MediaCommand::Play);
  }

  // When Clicked On: no play step; the click on it (its trigger) plays it; the media node stays.
  CHECK(ed.SetMediaStart(0, video, MediaStart::WhenClickedOn).ok);
  CHECK(ed.GetMediaStart(0, video, &s) && s == MediaStart::WhenClickedOn);
  t = SlideTiming(PartText(ed, part));
  CHECK(t.main.steps.empty() && t.triggers.size() == 1 && t.media.size() == 1);
  {
    fw::slide::show::Animator a(&t, nullptr, nullptr);
    a.Start(0);
    CHECK(a.TakeMediaEvents().empty());
    CHECK(a.Trigger(video, 10));
    std::vector<fw::slide::show::MediaEvent> ev = a.TakeMediaEvents();
    CHECK(ev.size() == 1 && ev[0].command == MediaCommand::TogglePause);
  }

  // Back In Click Sequence: a play step again, on a click.
  CHECK(ed.SetMediaStart(0, video, MediaStart::InClickSequence).ok);
  CHECK(ed.GetMediaStart(0, video, &s) && s == MediaStart::InClickSequence);
  t = SlideTiming(PartText(ed, part));
  CHECK(t.main.steps.size() == 1 && t.main.steps[0].start == fw::slide::show::StepStart::Click);
  CHECK(t.triggers.size() == 1 && t.media.size() == 1);
  CHECK(ed.SetMediaStart(0, video, MediaStart::InClickSequence).ok);  // no change: no step
  CHECK(ed.Undo(3).ok);
  CHECK(PartText(ed, part) == before);
}
