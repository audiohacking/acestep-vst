#pragma once

#include <functional>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

// Forward declaration so LibraryListModel / LibraryListBox can reference the editor.
class AcestepAudioProcessorEditor;

// ── Library list model ────────────────────────────────────────────────────────
// Reads from the editor's cached library to avoid repeated disk scans.
class LibraryListModel : public juce::ListBoxModel
{
public:
    explicit LibraryListModel(AcestepAudioProcessorEditor& e);
    int  getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    void setOnRowDoubleClicked(std::function<void(int)> f) { onRowDoubleClicked_ = std::move(f); }

private:
    AcestepAudioProcessorEditor& editor_;
    std::function<void(int)> onRowDoubleClicked_;
};

// ── Library list box (supports OS-level drag-to-DAW) ─────────────────────────
// Mouse events on list rows are delivered to JUCE's internal ContentComponent,
// not to the ListBox itself.  A DragHelper MouseListener registered with
// addMouseListener(&dragHelper_, true) ensures we receive those nested events.
class LibraryListBox : public juce::ListBox
{
public:
    LibraryListBox(AcestepAudioProcessorEditor& e, LibraryListModel& m);

private:
    AcestepAudioProcessorEditor& editor_;

    // DragHelper is a friend so it can reach editor_ and getCachedLibrary().
    struct DragHelper : public juce::MouseListener
    {
        explicit DragHelper(LibraryListBox& lb) : lb_(lb) {}
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp  (const juce::MouseEvent&)   override;

        LibraryListBox& lb_;
        int  dragRow_{ -1 };
        bool dragStarted_{ false };
    };

    friend struct DragHelper;
    DragHelper dragHelper_{ *this };
};

// ══════════════════════════════════════════════════════════════════════════════
// Main editor — 3-tab UI: Generate | Library | Settings
// Accepts files dropped from the OS (Finder / Explorer / DAW file browser).
// ══════════════════════════════════════════════════════════════════════════════
class AcestepAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    public juce::DragAndDropContainer,
                                    public juce::FileDragAndDropTarget,
                                    public juce::Timer
{
public:
    explicit AcestepAudioProcessorEditor(AcestepAudioProcessor&);
    ~AcestepAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;

    // FileDragAndDropTarget — accept audio files from OS
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    void fileDragEnter(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;

    // Library cache (LibraryListModel reads from here; avoids per-repaint disk scans)
    const std::vector<AcestepAudioProcessor::LibraryEntry>& getCachedLibrary() const
        { return libraryCache_; }
    void refreshLibraryCache();

    void showFeedback(const juce::String& msg, int ticks = 12);

private:
    AcestepAudioProcessor& processorRef;

    // ── Tab system ────────────────────────────────────────────────────────────
    enum Tab { TabGenerate = 0, TabLibrary = 1, TabSettings = 2 };
    int currentTab_{ TabGenerate };
    juce::TextButton tabGenerate_{ "Generate" };
    juce::TextButton tabLibrary_ { "Library"  };
    juce::TextButton tabSettings_{ "Settings" };
    void selectTab(int tab);

    // ── Shared / always-visible ───────────────────────────────────────────────
    juce::Label titleLabel_;
    juce::Label engineStatusLabel_;

    // ── Generate tab ──────────────────────────────────────────────────────────
    juce::Label      promptLabel_;
    juce::TextEditor promptEditor_;
    juce::Label      lyricsLabel_;
    juce::TextEditor lyricsEditor_;
    juce::Label      durationLabel_;
    juce::ComboBox   durationCombo_;
    juce::Label      stepsLabel_;
    juce::ComboBox   stepsCombo_;
    juce::Label      bpmLabel_;
    juce::TextEditor bpmEditor_;
    juce::Label      seedLabel_;
    juce::TextEditor seedEditor_;
    bool             bpmAutoUpdated_{ true };

    // Mode buttons — Text-to-Music, Cover, or Lego
    juce::TextButton genModeButton_  { "Text-to-Music" };
    juce::TextButton coverModeButton_{ "Cover Mode"    };
    juce::TextButton legoModeButton_ { "Lego"          };

    enum class GenerationMode { TextToMusic, Cover, Lego };
    GenerationMode generationMode_{ GenerationMode::TextToMusic };

    // Cover-mode-only controls
    juce::Label      refFileLabel_;
    juce::TextButton browseRefButton_{ "Browse\xe2\x80\xa6" };
    juce::TextButton clearRefButton_ { "Clear" };
    juce::String     referenceAudioPath_;
    juce::Label      coverStrengthLabel_;
    juce::Slider     coverStrengthSlider_;

    // Lego-mode-only controls
    juce::Label    legoTrackLabel_;
    juce::ComboBox legoTrackCombo_;

    // Model selector (applies to text-to-music and cover; lego always uses Base)
    juce::Label    modelLabel_;
    juce::ComboBox modelCombo_;

    juce::TextButton generateButton_{ "Generate" };
    // Read-only multi-line log that shows all subprocess output (ace-lm / ace-synth)
    // and status messages; autoscrolls to the last line on each update.
    juce::TextEditor logEditor_;

    // ── Library tab ───────────────────────────────────────────────────────────
    juce::TextButton refreshLibButton_{ "Refresh" };
    juce::TextButton importButton_    { "Import File\xe2\x80\xa6" };
    LibraryListModel libraryListModel_;
    LibraryListBox   libraryList_;

    juce::TextButton previewButton_  { "\xe2\x96\xb6 Preview"       };
    juce::TextButton stopButton_     { "\xe2\x96\xa0 Stop"          };
    juce::TextButton loopButton_     { "\xe2\x9f\xb3 Loop"          };
    juce::TextButton deleteButton_   { "Delete"                      };
    juce::TextButton useAsRefButton_ { "Use as Reference"            };
    juce::TextButton insertDawButton_{ "Insert into DAW"             };
#if JUCE_MAC
    juce::TextButton revealButton_   { "Reveal in Finder"            };
#elif JUCE_WINDOWS
    juce::TextButton revealButton_   { "Reveal in Explorer"          };
#else
    juce::TextButton revealButton_   { "Reveal in Files"             };
#endif
    juce::Label      libHintLabel_;

    bool fileDragHighlight_{ false };

    // ── Settings tab ──────────────────────────────────────────────────────────
    juce::Label      binPathLabel_;
    juce::Label      binDetectedLabel_;   // shows the auto-detected bundled binary dir
    juce::TextEditor binPathEditor_;
    juce::TextButton binBrowseButton_   { "Browse\xe2\x80\xa6" };
    juce::Label      modelsPathLabel_;
    juce::TextEditor modelsPathEditor_;
    juce::TextButton modelsBrowseButton_{ "Browse\xe2\x80\xa6" };
    juce::Label      outputPathLabel_;
    juce::TextEditor outputPathEditor_;
    juce::TextButton outputBrowseButton_{ "Browse\xe2\x80\xa6" };
    juce::Label      audioFormatLabel_;
    juce::ComboBox   audioFormatCombo_;
    juce::TextButton applySettingsButton_{ "Apply Settings" };
    juce::Label      settingsInfoLabel_;

    // ── Library cache ─────────────────────────────────────────────────────────
    std::vector<AcestepAudioProcessor::LibraryEntry> libraryCache_;
    int  libraryRefreshTick_{ 0 };
    AcestepAudioProcessor::State lastState_{ AcestepAudioProcessor::State::Idle };

    // ── Feedback ──────────────────────────────────────────────────────────────
    juce::String feedbackMsg_;
    int          feedbackCountdown_{ 0 };

    // ── Insert-DAW drag helper ────────────────────────────────────────────────
    // performExternalDragDropOfFiles() must be called from a mouseDrag event,
    // not from onClick (mouse-up).  This listener watches the button and starts
    // the OS-level file drag as soon as the user drags it far enough.
    struct InsertDawDragListener : public juce::MouseListener
    {
        explicit InsertDawDragListener(AcestepAudioProcessorEditor& e) : editor_(e) {}
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp  (const juce::MouseEvent&)   override { dragStarted_ = false; }
        AcestepAudioProcessorEditor& editor_;
        bool dragStarted_{ false };
    };
    InsertDawDragListener insertDawDragListener_{ *this };

    // ── Internal actions ──────────────────────────────────────────────────────
    void onGenerateClicked();
    void onPreviewClicked();
    void onStopClicked();
    void onDeleteClicked();
    void onUseAsRefClicked();
    void onImportClicked();
    void onInsertDawClicked();
    void startInsertDawDrag();
    void onRevealClicked();
    void onApplySettingsClicked();
    void onBrowseRefClicked();
    void setGenerationMode(GenerationMode mode);
    void updateStatusFromProcessor();

    // ── Layout helpers ────────────────────────────────────────────────────────
    void hideAllTabComponents();
    void layoutGenerateTab(juce::Rectangle<int> r);
    void layoutLibraryTab (juce::Rectangle<int> r);
    void layoutSettingsTab(juce::Rectangle<int> r);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AcestepAudioProcessorEditor)
};
