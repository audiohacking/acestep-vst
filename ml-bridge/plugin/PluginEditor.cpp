#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <juce_core/juce_core.h>

// ─────────────────────────────────────────────────────────────────────────────
// Colour palette
// ─────────────────────────────────────────────────────────────────────────────
namespace AcestepColours
{
static const juce::Colour bg        { 0xff12121e };
static const juce::Colour panel     { 0xff1c1c2e };
static const juce::Colour accent    { 0xff5b5bff };
static const juce::Colour accentDim { 0xff2e2e7a };
static const juce::Colour textMain  { 0xffeeeeee };
static const juce::Colour textDim   { 0xffaaaaaa };
static const juce::Colour listSel   { 0xff2a2a55 };
static const juce::Colour ok        { 0xff55cc88 };
static const juce::Colour err       { 0xffff6655 };
} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Helper: configure a small text button
// ─────────────────────────────────────────────────────────────────────────────
static void styleSmallButton(juce::TextButton& b,
                              juce::Colour bg    = AcestepColours::accentDim,
                              juce::Colour hover = AcestepColours::accent)
{
    b.setColour(juce::TextButton::buttonColourId,   bg);
    b.setColour(juce::TextButton::buttonOnColourId, hover);
    b.setColour(juce::TextButton::textColourOffId,  AcestepColours::textMain);
    b.setColour(juce::TextButton::textColourOnId,   AcestepColours::textMain);
}

static void styleLabel(juce::Label& l, float size = 13.0f,
                       juce::Colour colour = AcestepColours::textMain)
{
    l.setColour(juce::Label::textColourId, colour);
    l.setFont(juce::Font(juce::FontOptions().withPointHeight(size)));
    l.setJustificationType(juce::Justification::centredLeft);
}

// ─────────────────────────────────────────────────────────────────────────────
// LibraryListModel
// ─────────────────────────────────────────────────────────────────────────────

LibraryListModel::LibraryListModel(AcestepAudioProcessorEditor& e) : editor_(e) {}

int LibraryListModel::getNumRows()
{
    return static_cast<int>(editor_.getCachedLibrary().size());
}

void LibraryListModel::paintListBoxItem(int row, juce::Graphics& g,
                                        int width, int height, bool selected)
{
    const auto& entries = editor_.getCachedLibrary();
    if (row < 0 || row >= static_cast<int>(entries.size())) return;
    const auto& e = entries[static_cast<size_t>(row)];

    if (selected) g.fillAll(AcestepColours::listSel);
    else          g.fillAll(AcestepColours::panel);

    // File name / prompt
    juce::String name = e.prompt.isNotEmpty()
        ? e.prompt.substring(0, 60)
        : e.file.getFileNameWithoutExtension();

    g.setColour(AcestepColours::textMain);
    g.setFont(juce::Font(juce::FontOptions().withPointHeight(13.0f)));
    g.drawText(name, 8, 0, width - 100, height, juce::Justification::centredLeft, true);

    // Date + extension
    g.setColour(AcestepColours::textDim);
    g.setFont(juce::Font(juce::FontOptions().withPointHeight(11.0f)));
    juce::String meta = e.file.getFileExtension().toUpperCase().trimCharactersAtStart(".")
                      + "  " + e.time.formatted("%m/%d %H:%M");
    g.drawText(meta, width - 96, 0, 92, height, juce::Justification::centredRight);
}

void LibraryListModel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (onRowDoubleClicked_) onRowDoubleClicked_(row);
}

// ─────────────────────────────────────────────────────────────────────────────
// LibraryListBox
// ─────────────────────────────────────────────────────────────────────────────

LibraryListBox::LibraryListBox(AcestepAudioProcessorEditor& e, LibraryListModel& m)
    : ListBox("Library", &m), editor_(e)
{
    setRowHeight(28);
    setOutlineThickness(0);
    setColour(juce::ListBox::backgroundColourId, AcestepColours::panel);
}

void LibraryListBox::mouseDrag(const juce::MouseEvent& e)
{
    if (dragStarted_) { ListBox::mouseDrag(e); return; }
    if (e.getDistanceFromDragStart() < 10) { ListBox::mouseDrag(e); return; }

    const int row = getRowContainingPosition(e.x, e.y);
    const auto& entries = editor_.getCachedLibrary();
    if (row < 0 || row >= static_cast<int>(entries.size())) { ListBox::mouseDrag(e); return; }

    juce::String path = entries[static_cast<size_t>(row)].file.getFullPathName();
    if (path.isEmpty()) { ListBox::mouseDrag(e); return; }

    auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this);
    auto* editorComp = findParentComponentOfClass<AcestepAudioProcessorEditor>();
    juce::Component* src = editorComp ? static_cast<juce::Component*>(editorComp)
                                      : static_cast<juce::Component*>(this);
    if (container && container->performExternalDragDropOfFiles(juce::StringArray(path), false, src))
        dragStarted_ = true;
    else
        ListBox::mouseDrag(e);
}

void LibraryListBox::mouseUp(const juce::MouseEvent& e)
{
    dragStarted_ = false;
    ListBox::mouseUp(e);
}

// ─────────────────────────────────────────────────────────────────────────────
// Editor — construction
// ─────────────────────────────────────────────────────────────────────────────

AcestepAudioProcessorEditor::AcestepAudioProcessorEditor(AcestepAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p),
      libraryListModel_(*this), libraryList_(*this, libraryListModel_)
{
    setSize(500, 660);

    // ── Tab buttons ──────────────────────────────────────────────────────────
    for (auto* b : { &tabGenerate_, &tabLibrary_, &tabSettings_ })
    {
        b->setClickingTogglesState(false);
        styleSmallButton(*b, AcestepColours::accentDim, AcestepColours::accent);
        addAndMakeVisible(b);
    }
    tabGenerate_.onClick = [this] { selectTab(TabGenerate); };
    tabLibrary_.onClick  = [this] { selectTab(TabLibrary);  };
    tabSettings_.onClick = [this] { selectTab(TabSettings); };

    // ── Engine status (visible across all tabs) ───────────────────────────────
    styleLabel(engineStatusLabel_, 12.0f, AcestepColours::textDim);
    addAndMakeVisible(engineStatusLabel_);

    // ── Generate tab components ───────────────────────────────────────────────
    styleLabel(promptLabel_); promptLabel_.setText("Prompt:", juce::dontSendNotification);
    addChildComponent(promptLabel_);

    promptEditor_.setMultiLine(false);
    promptEditor_.setReturnKeyStartsNewLine(false);
    promptEditor_.setText("upbeat electronic beat");
    promptEditor_.setTextToShowWhenEmpty("Describe the music (e.g. calm piano melody, 10s)",
                                         AcestepColours::textDim);
    promptEditor_.setColour(juce::TextEditor::backgroundColourId,  AcestepColours::panel);
    promptEditor_.setColour(juce::TextEditor::textColourId,        AcestepColours::textMain);
    promptEditor_.setColour(juce::TextEditor::outlineColourId,     AcestepColours::accentDim);
    promptEditor_.setColour(juce::TextEditor::focusedOutlineColourId, AcestepColours::accent);
    addChildComponent(promptEditor_);

    styleLabel(durationLabel_); durationLabel_.setText("Duration:", juce::dontSendNotification);
    addChildComponent(durationLabel_);
    durationCombo_.addItem("10 s", 10); durationCombo_.addItem("15 s", 15);
    durationCombo_.addItem("20 s", 20); durationCombo_.addItem("30 s", 30);
    durationCombo_.addItem("60 s", 60);
    durationCombo_.setSelectedId(10, juce::dontSendNotification);
    durationCombo_.setColour(juce::ComboBox::backgroundColourId, AcestepColours::panel);
    durationCombo_.setColour(juce::ComboBox::textColourId,       AcestepColours::textMain);
    addChildComponent(durationCombo_);

    styleLabel(stepsLabel_); stepsLabel_.setText("Quality:", juce::dontSendNotification);
    addChildComponent(stepsLabel_);
    stepsCombo_.addItem("Draft (8 steps)",    8);
    stepsCombo_.addItem("Fast (15 steps)",   15);
    stepsCombo_.addItem("Balanced (30 steps)",30);
    stepsCombo_.addItem("High (55 steps)",   55);
    stepsCombo_.setSelectedId(8, juce::dontSendNotification);
    stepsCombo_.setColour(juce::ComboBox::backgroundColourId, AcestepColours::panel);
    stepsCombo_.setColour(juce::ComboBox::textColourId,       AcestepColours::textMain);
    addChildComponent(stepsCombo_);

    styleLabel(bpmLabel_); bpmLabel_.setText("BPM:", juce::dontSendNotification);
    addChildComponent(bpmLabel_);
    bpmEditor_.setInputRestrictions(0, "0123456789.");
    bpmEditor_.setTextToShowWhenEmpty("auto from DAW", AcestepColours::textDim);
    bpmEditor_.setColour(juce::TextEditor::backgroundColourId, AcestepColours::panel);
    bpmEditor_.setColour(juce::TextEditor::textColourId,       AcestepColours::textMain);
    bpmEditor_.setColour(juce::TextEditor::outlineColourId,    AcestepColours::accentDim);
    bpmEditor_.onTextChange = [this] { bpmAutoUpdated_ = false; };
    addChildComponent(bpmEditor_);

    styleSmallButton(genModeButton_, AcestepColours::accent, AcestepColours::accentDim);
    addChildComponent(genModeButton_);
    styleSmallButton(coverModeButton_, AcestepColours::accentDim, AcestepColours::accent);
    addChildComponent(coverModeButton_);
    genModeButton_.onClick   = [this] { setCoverMode(false); };
    coverModeButton_.onClick = [this] { setCoverMode(true);  };

    // Cover-only controls
    styleLabel(refFileLabel_, 11.0f, AcestepColours::textDim);
    refFileLabel_.setText("No reference file", juce::dontSendNotification);
    addChildComponent(refFileLabel_);
    styleSmallButton(browseRefButton_);
    addChildComponent(browseRefButton_);
    browseRefButton_.onClick = [this] { onBrowseRefClicked(); };
    styleSmallButton(clearRefButton_, juce::Colour(0xff663333), juce::Colour(0xffaa4444));
    addChildComponent(clearRefButton_);
    clearRefButton_.onClick = [this] {
        referenceAudioPath_.clear();
        refFileLabel_.setText("No reference file", juce::dontSendNotification);
    };

    styleLabel(coverStrengthLabel_);
    coverStrengthLabel_.setText("Cover strength:", juce::dontSendNotification);
    addChildComponent(coverStrengthLabel_);
    coverStrengthSlider_.setRange(0.0, 1.0, 0.01);
    coverStrengthSlider_.setValue(0.5, juce::dontSendNotification);
    coverStrengthSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    coverStrengthSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
    coverStrengthSlider_.setColour(juce::Slider::thumbColourId,           AcestepColours::accent);
    coverStrengthSlider_.setColour(juce::Slider::trackColourId,           AcestepColours::accentDim);
    coverStrengthSlider_.setColour(juce::Slider::textBoxTextColourId,     AcestepColours::textMain);
    coverStrengthSlider_.setColour(juce::Slider::textBoxBackgroundColourId, AcestepColours::panel);
    addChildComponent(coverStrengthSlider_);

    styleSmallButton(generateButton_, AcestepColours::accent,
                     AcestepColours::accent.brighter(0.2f));
    generateButton_.setButtonText("Generate");
    generateButton_.onClick = [this] { onGenerateClicked(); };
    addChildComponent(generateButton_);

    statusLabel_.setColour(juce::Label::textColourId, AcestepColours::textDim);
    statusLabel_.setFont(juce::Font(juce::FontOptions().withPointHeight(12.0f)));
    statusLabel_.setJustificationType(juce::Justification::topLeft);
    statusLabel_.setMinimumHorizontalScale(1.0f);
    addChildComponent(statusLabel_);

    // ── Library tab components ────────────────────────────────────────────────
    styleSmallButton(refreshLibButton_);
    addChildComponent(refreshLibButton_);
    refreshLibButton_.onClick = [this] { refreshLibraryCache(); libraryList_.updateContent(); };

    styleSmallButton(importButton_);
    addChildComponent(importButton_);
    importButton_.onClick = [this] { onImportClicked(); };

    addChildComponent(libraryList_);

    for (auto* b : { &previewButton_, &stopButton_, &loopButton_,
                     &deleteButton_, &useAsRefButton_,
                     &insertDawButton_, &revealButton_ })
    {
        styleSmallButton(*b);
        addChildComponent(b);
    }
    previewButton_.onClick   = [this] { onPreviewClicked();   };
    stopButton_.onClick      = [this] { onStopClicked();      };
    loopButton_.setClickingTogglesState(true);
    loopButton_.onClick      = [this] {
        processorRef.setLoopPlayback(loopButton_.getToggleState());
    };
    deleteButton_.onClick    = [this] { onDeleteClicked();    };
    useAsRefButton_.onClick  = [this] { onUseAsRefClicked();  };
    insertDawButton_.onClick = [this] { onInsertDawClicked(); };
    revealButton_.onClick    = [this] { onRevealClicked();    };

    libHintLabel_.setColour(juce::Label::textColourId, AcestepColours::textDim);
    libHintLabel_.setFont(juce::Font(juce::FontOptions().withPointHeight(10.5f)));
    libHintLabel_.setMinimumHorizontalScale(1.0f);
    libHintLabel_.setText("Drag a row to insert into your DAW timeline. "
                           "Double-click to copy the path.",
                           juce::dontSendNotification);
    addChildComponent(libHintLabel_);

    libraryListModel_.setOnRowDoubleClicked([this](int row)
    {
        const auto& entries = getCachedLibrary();
        if (row < 0 || row >= static_cast<int>(entries.size())) return;
        juce::SystemClipboard::copyTextToClipboard(
            entries[static_cast<size_t>(row)].file.getFullPathName());
        showFeedback("Path copied to clipboard.");
    });

    // ── Settings tab components ────────────────────────────────────────────────
    styleLabel(binPathLabel_);
    binPathLabel_.setText("Binaries directory (ace-qwen3, dit-vae):", juce::dontSendNotification);
    addChildComponent(binPathLabel_);

    for (auto* te : { &binPathEditor_, &modelsPathEditor_, &outputPathEditor_ })
    {
        te->setColour(juce::TextEditor::backgroundColourId, AcestepColours::panel);
        te->setColour(juce::TextEditor::textColourId,       AcestepColours::textMain);
        te->setColour(juce::TextEditor::outlineColourId,    AcestepColours::accentDim);
        addChildComponent(te);
    }
    binPathEditor_.setTextToShowWhenEmpty(
        "Default: next to plugin bundle", AcestepColours::textDim);
    for (auto* b : { &binBrowseButton_, &modelsBrowseButton_, &outputBrowseButton_ })
    { styleSmallButton(*b); addChildComponent(b); }

    binBrowseButton_.onClick = [this]
    {
        juce::FileChooser fc("Select binaries directory",
                             juce::File(binPathEditor_.getText()));
        if (fc.browseForDirectory())
            binPathEditor_.setText(fc.getResult().getFullPathName(),
                                   juce::dontSendNotification);
    };

    styleLabel(modelsPathLabel_);
    modelsPathLabel_.setText("Models directory (*.gguf files):", juce::dontSendNotification);
    addChildComponent(modelsPathLabel_);
    modelsPathEditor_.setTextToShowWhenEmpty(
        "Default: ~/Library/Application Support/AcestepVST/models",
        AcestepColours::textDim);
    modelsBrowseButton_.onClick = [this]
    {
        juce::FileChooser fc("Select models directory",
                             juce::File(modelsPathEditor_.getText()));
        if (fc.browseForDirectory())
            modelsPathEditor_.setText(fc.getResult().getFullPathName(),
                                      juce::dontSendNotification);
    };

    styleLabel(outputPathLabel_);
    outputPathLabel_.setText("Output / Generations directory:", juce::dontSendNotification);
    addChildComponent(outputPathLabel_);
    outputPathEditor_.setTextToShowWhenEmpty(
        "Default: ~/Library/Application Support/AcestepVST/Generations",
        AcestepColours::textDim);
    outputBrowseButton_.onClick = [this]
    {
        juce::FileChooser fc("Select output directory",
                             juce::File(outputPathEditor_.getText()));
        if (fc.browseForDirectory())
            outputPathEditor_.setText(fc.getResult().getFullPathName(),
                                      juce::dontSendNotification);
    };

    styleSmallButton(applySettingsButton_, AcestepColours::accent,
                     AcestepColours::accent.brighter(0.2f));
    applySettingsButton_.onClick = [this] { onApplySettingsClicked(); };
    addChildComponent(applySettingsButton_);

    settingsInfoLabel_.setColour(juce::Label::textColourId, AcestepColours::textDim);
    settingsInfoLabel_.setFont(juce::Font(juce::FontOptions().withPointHeight(11.0f)));
    settingsInfoLabel_.setMinimumHorizontalScale(1.0f);
    settingsInfoLabel_.setJustificationType(juce::Justification::topLeft);
    settingsInfoLabel_.setText(
        "Leave fields empty to use the defaults shown as hints.\n"
        "Build binaries: cmake -B vendor/acestep.cpp/build vendor/acestep.cpp\n"
        "              && cmake --build vendor/acestep.cpp/build --config Release\n"
        "Download models: cd vendor/acestep.cpp && ./models.sh",
        juce::dontSendNotification);
    addChildComponent(settingsInfoLabel_);

    // ── Initial state ────────────────────────────────────────────────────────
    // Populate settings fields from processor state
    binPathEditor_.setText(processorRef.getBinariesPath(), juce::dontSendNotification);
    modelsPathEditor_.setText(processorRef.getModelsPath(), juce::dontSendNotification);
    outputPathEditor_.setText(processorRef.getOutputPath(), juce::dontSendNotification);

    refreshLibraryCache();
    selectTab(TabGenerate);
    startTimerHz(4);
}

AcestepAudioProcessorEditor::~AcestepAudioProcessorEditor()
{
    stopTimer();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tab system
// ─────────────────────────────────────────────────────────────────────────────

void AcestepAudioProcessorEditor::selectTab(int tab)
{
    currentTab_ = tab;
    tabGenerate_.setColour(juce::TextButton::buttonColourId,
        tab == TabGenerate ? AcestepColours::accent : AcestepColours::accentDim);
    tabLibrary_.setColour(juce::TextButton::buttonColourId,
        tab == TabLibrary  ? AcestepColours::accent : AcestepColours::accentDim);
    tabSettings_.setColour(juce::TextButton::buttonColourId,
        tab == TabSettings ? AcestepColours::accent : AcestepColours::accentDim);

    if (tab == TabLibrary)
    {
        refreshLibraryCache();
        libraryList_.updateContent();
    }
    if (tab == TabSettings)
    {
        // Sync settings fields from processor
        binPathEditor_.setText(processorRef.getBinariesPath(), juce::dontSendNotification);
        modelsPathEditor_.setText(processorRef.getModelsPath(), juce::dontSendNotification);
        outputPathEditor_.setText(processorRef.getOutputPath(), juce::dontSendNotification);
    }
    resized();
    repaint();
}

void AcestepAudioProcessorEditor::setCoverMode(bool cover)
{
    coverModeActive_ = cover;
    genModeButton_.setColour(juce::TextButton::buttonColourId,
        !cover ? AcestepColours::accent : AcestepColours::accentDim);
    coverModeButton_.setColour(juce::TextButton::buttonColourId,
        cover  ? AcestepColours::accent : AcestepColours::accentDim);
    resized();
    repaint();
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer — update UI from processor state
// ─────────────────────────────────────────────────────────────────────────────

void AcestepAudioProcessorEditor::timerCallback()
{
    // Auto-update BPM from host if user hasn't manually set it
    if (bpmAutoUpdated_ && currentTab_ == TabGenerate)
    {
        const double bpm = processorRef.getHostBpm();
        if (bpm > 0.0)
        {
            const juce::String bpmStr = juce::String(juce::roundToInt(bpm));
            if (bpmEditor_.getText() != bpmStr)
                bpmEditor_.setText(bpmStr, juce::dontSendNotification);
        }
    }

    // Loop button reflects processor state
    if (loopButton_.getToggleState() != processorRef.isLoopPlayback())
        loopButton_.setToggleState(processorRef.isLoopPlayback(), juce::dontSendNotification);

    // Refresh library cache once per second (4 Hz timer → every 4 ticks)
    ++libraryRefreshTick_;
    if (libraryRefreshTick_ >= 4)
    {
        libraryRefreshTick_ = 0;
        refreshLibraryCache();
        if (currentTab_ == TabLibrary) libraryList_.updateContent();
    }

    updateStatusFromProcessor();
}

void AcestepAudioProcessorEditor::updateStatusFromProcessor()
{
    const auto state = processorRef.getState();

    // Engine readiness
    if (processorRef.areBinariesReady())
        engineStatusLabel_.setText("Engine: ready", juce::dontSendNotification);
    else if (state == AcestepAudioProcessor::State::Failed)
        engineStatusLabel_.setText("Engine: error \xe2\x80\x94 see status", juce::dontSendNotification);
    else
        engineStatusLabel_.setText("Engine: binaries not found \xe2\x80\x94 see Settings",
                                   juce::dontSendNotification);

    // Status label (with temporary feedback messages)
    if (feedbackCountdown_ > 0)
    {
        statusLabel_.setText(feedbackMsg_, juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId, AcestepColours::ok);
        --feedbackCountdown_;
    }
    else
    {
        statusLabel_.setText(processorRef.getStatusText(), juce::dontSendNotification);
        statusLabel_.setColour(juce::Label::textColourId,
            state == AcestepAudioProcessor::State::Failed
            ? AcestepColours::err : AcestepColours::textDim);
    }

    // Generate / preview buttons: disable while engine is busy
    const bool busy = (state == AcestepAudioProcessor::State::Submitting
                    || state == AcestepAudioProcessor::State::Running);
    generateButton_.setEnabled(!busy);
    previewButton_.setEnabled(!busy && libraryList_.getSelectedRow() >= 0);
    deleteButton_.setEnabled(libraryList_.getSelectedRow() >= 0);
    useAsRefButton_.setEnabled(libraryList_.getSelectedRow() >= 0);
    insertDawButton_.setEnabled(libraryList_.getSelectedRow() >= 0);
    revealButton_.setEnabled(libraryList_.getSelectedRow() >= 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Library cache
// ─────────────────────────────────────────────────────────────────────────────

void AcestepAudioProcessorEditor::refreshLibraryCache()
{
    libraryCache_ = processorRef.getLibraryEntries();
}

// ─────────────────────────────────────────────────────────────────────────────
// Actions
// ─────────────────────────────────────────────────────────────────────────────

void AcestepAudioProcessorEditor::onGenerateClicked()
{
    const int durationSec = durationCombo_.getSelectedId();
    const int steps       = stepsCombo_.getSelectedId();
    const float bpm       = bpmEditor_.getText().getFloatValue();

    juce::File coverFile;
    float coverStrength = 0.5f;
    if (coverModeActive_ && referenceAudioPath_.isNotEmpty())
    {
        coverFile     = juce::File(referenceAudioPath_);
        coverStrength = static_cast<float>(coverStrengthSlider_.getValue());
    }

    processorRef.startGeneration(
        promptEditor_.getText(),
        durationSec > 0 ? durationSec : 10,
        steps       > 0 ? steps       : 8,
        coverFile,
        coverStrength,
        bpm > 0.0f ? bpm : static_cast<float>(processorRef.getHostBpm()));
}

void AcestepAudioProcessorEditor::onPreviewClicked()
{
    const int row = libraryList_.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(libraryCache_.size())) return;
    processorRef.previewLibraryEntry(libraryCache_[static_cast<size_t>(row)].file);
}

void AcestepAudioProcessorEditor::onStopClicked()
{
    processorRef.stopPlayback();
}

void AcestepAudioProcessorEditor::onDeleteClicked()
{
    const int row = libraryList_.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(libraryCache_.size()))
    {
        showFeedback("Select an entry first.");
        return;
    }
    const juce::File file = libraryCache_[static_cast<size_t>(row)].file;
    if (processorRef.deleteLibraryEntry(file))
    {
        refreshLibraryCache();
        libraryList_.updateContent();
        showFeedback("Deleted: " + file.getFileName());
    }
    else
    {
        showFeedback("Delete failed.");
    }
}

void AcestepAudioProcessorEditor::onUseAsRefClicked()
{
    const int row = libraryList_.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(libraryCache_.size()))
    {
        showFeedback("Select a library entry first.");
        return;
    }
    referenceAudioPath_ = libraryCache_[static_cast<size_t>(row)].file.getFullPathName();
    refFileLabel_.setText(libraryCache_[static_cast<size_t>(row)].file.getFileName(),
                          juce::dontSendNotification);
    setCoverMode(true);
    selectTab(TabGenerate);
    showFeedback("Reference set \xe2\x80\x94 switched to Cover Mode.");
}

void AcestepAudioProcessorEditor::onImportClicked()
{
    juce::FileChooser fc("Import audio file",
                         juce::File::getSpecialLocation(juce::File::userHomeDirectory),
                         "*.wav;*.mp3");
    if (fc.browseForFileToOpen())
    {
        if (processorRef.importAudioFile(fc.getResult()))
        {
            refreshLibraryCache();
            libraryList_.updateContent();
            showFeedback("Imported: " + fc.getResult().getFileName());
        }
        else
        {
            showFeedback("Import failed (only WAV and MP3 are supported).");
        }
    }
}

void AcestepAudioProcessorEditor::onInsertDawClicked()
{
    const int row = libraryList_.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(libraryCache_.size()))
    {
        showFeedback("Select a library entry first."); return;
    }
    const juce::File& file = libraryCache_[static_cast<size_t>(row)].file;
    if (!file.existsAsFile()) { showFeedback("File not found."); return; }

    juce::SystemClipboard::copyTextToClipboard(file.getFullPathName());

#if JUCE_MAC
    juce::ChildProcess proc;
    juce::StringArray args;
    args.add("open"); args.add("-a"); args.add("Logic Pro"); args.add(file.getFullPathName());
    if (proc.start(args, 0))
        showFeedback("Opened in Logic Pro. Drag from there into your project.");
    else
        showFeedback("Path copied. Use Reveal in Finder and drag to your DAW.");
#else
    showFeedback("Path copied to clipboard. Drag into your DAW manually.");
#endif
}

void AcestepAudioProcessorEditor::onRevealClicked()
{
    const int row = libraryList_.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(libraryCache_.size()))
    {
        showFeedback("Select a library entry first."); return;
    }
    const juce::File& f = libraryCache_[static_cast<size_t>(row)].file;
    if (f.existsAsFile()) f.revealToUser();
    else showFeedback("File not found.");
}

void AcestepAudioProcessorEditor::onBrowseRefClicked()
{
    juce::FileChooser fc("Select reference audio",
                         juce::File::getSpecialLocation(juce::File::userHomeDirectory),
                         "*.wav;*.mp3");
    if (fc.browseForFileToOpen())
    {
        referenceAudioPath_ = fc.getResult().getFullPathName();
        refFileLabel_.setText(fc.getResult().getFileName(), juce::dontSendNotification);
    }
}

void AcestepAudioProcessorEditor::onApplySettingsClicked()
{
    processorRef.setBinariesPath(binPathEditor_.getText().trim());
    processorRef.setModelsPath(modelsPathEditor_.getText().trim());
    processorRef.setOutputPath(outputPathEditor_.getText().trim());
    showFeedback("Settings applied.");
    // Refresh library in case output path changed
    refreshLibraryCache();
    if (currentTab_ == TabLibrary) libraryList_.updateContent();
}

void AcestepAudioProcessorEditor::showFeedback(const juce::String& msg, int ticks)
{
    feedbackMsg_       = msg;
    feedbackCountdown_ = ticks;
}

// ─────────────────────────────────────────────────────────────────────────────
// File drag-and-drop from OS
// ─────────────────────────────────────────────────────────────────────────────

bool AcestepAudioProcessorEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
    {
        juce::String ext = juce::File(f).getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".mp3") return true;
    }
    return false;
}

void AcestepAudioProcessorEditor::fileDragEnter(const juce::StringArray&, int, int)
{
    fileDragHighlight_ = true;
    repaint();
}

void AcestepAudioProcessorEditor::fileDragExit(const juce::StringArray&)
{
    fileDragHighlight_ = false;
    repaint();
}

void AcestepAudioProcessorEditor::filesDropped(const juce::StringArray& files, int, int)
{
    fileDragHighlight_ = false;
    repaint();

    // Filter to supported audio types
    juce::StringArray audioFiles;
    for (const auto& f : files)
    {
        juce::String ext = juce::File(f).getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".mp3") audioFiles.add(f);
    }
    if (audioFiles.isEmpty()) return;

    const juce::File dropped(audioFiles[0]);

    if (currentTab_ == TabGenerate)
    {
        // Set as cover reference
        referenceAudioPath_ = dropped.getFullPathName();
        refFileLabel_.setText(dropped.getFileName(), juce::dontSendNotification);
        setCoverMode(true);
        showFeedback("Reference set: " + dropped.getFileName() + " \xe2\x80\x94 switched to Cover Mode.");
    }
    else if (currentTab_ == TabLibrary)
    {
        // Import into library; report per-file failures
        int imported = 0;
        juce::StringArray failed;
        for (const auto& path : audioFiles)
        {
            if (processorRef.importAudioFile(juce::File(path)))
                ++imported;
            else
                failed.add(juce::File(path).getFileName());
        }
        refreshLibraryCache();
        libraryList_.updateContent();
        if (failed.isEmpty())
            showFeedback("Imported " + juce::String(imported) + " file(s) into the library.");
        else
            showFeedback("Imported " + juce::String(imported) + "; failed: "
                       + failed.joinIntoString(", "));
    }
    else
    {
        // Default: import
        if (processorRef.importAudioFile(dropped))
        {
            refreshLibraryCache();
            showFeedback("Imported: " + dropped.getFileName());
        }
        else
        {
            showFeedback("Import failed: " + dropped.getFileName()
                       + " (only WAV and MP3 are supported).");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Paint
// ─────────────────────────────────────────────────────────────────────────────

void AcestepAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(AcestepColours::bg);

    // Title bar area
    g.setColour(AcestepColours::panel);
    g.fillRect(0, 0, getWidth(), 48);

    // Title text
    g.setColour(AcestepColours::textMain);
    g.setFont(juce::Font(juce::FontOptions().withPointHeight(18.0f).withStyle("Bold")));
    g.drawText("ACE-Step", 12, 4, 200, 26, juce::Justification::centredLeft);

    // File-drag-over highlight
    if (fileDragHighlight_)
    {
        g.setColour(juce::Colour(0x3300aaff));
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(3.0f), 8.0f);
        g.setColour(juce::Colour(0xff00aaff));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(3.0f), 8.0f, 2.0f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Resized — layout all components
// ─────────────────────────────────────────────────────────────────────────────

void AcestepAudioProcessorEditor::hideAllTabComponents()
{
    for (auto* c : {
        (juce::Component*)&promptLabel_, &promptEditor_,
        &durationLabel_, &durationCombo_, &stepsLabel_, &stepsCombo_,
        &bpmLabel_, &bpmEditor_,
        &genModeButton_, &coverModeButton_,
        &refFileLabel_, &browseRefButton_, &clearRefButton_,
        &coverStrengthLabel_, &coverStrengthSlider_,
        &generateButton_, &statusLabel_,
        &refreshLibButton_, &importButton_,
        &libraryList_,
        &previewButton_, &stopButton_, &loopButton_,
        &deleteButton_, &useAsRefButton_,
        &insertDawButton_, &revealButton_,
        &libHintLabel_,
        &binPathLabel_, &binPathEditor_, &binBrowseButton_,
        &modelsPathLabel_, &modelsPathEditor_, &modelsBrowseButton_,
        &outputPathLabel_, &outputPathEditor_, &outputBrowseButton_,
        &applySettingsButton_, &settingsInfoLabel_ })
    {
        c->setVisible(false);
    }
}

void AcestepAudioProcessorEditor::resized()
{
    const int W   = getWidth();
    const int pad = 10;

    // ── Header (title + engine status) ───────────────────────────────────────
    engineStatusLabel_.setBounds(pad, 28, W - 2 * pad, 16);
    engineStatusLabel_.setVisible(true);

    // ── Tab buttons ───────────────────────────────────────────────────────────
    const int tabY = 48, tabH = 26, tabW = (W - 2 * pad) / 3;
    tabGenerate_.setBounds(pad,                tabY, tabW,     tabH);
    tabLibrary_ .setBounds(pad + tabW,         tabY, tabW,     tabH);
    tabSettings_.setBounds(pad + tabW * 2,     tabY, W - pad - pad - tabW * 2, tabH);

    // ── Content area ──────────────────────────────────────────────────────────
    const int contentY = tabY + tabH + 6;
    auto contentRect   = juce::Rectangle<int>(pad, contentY, W - 2 * pad, getHeight() - contentY - pad);

    hideAllTabComponents();

    switch (currentTab_)
    {
    case TabGenerate: layoutGenerateTab(contentRect); break;
    case TabLibrary:  layoutLibraryTab (contentRect); break;
    case TabSettings: layoutSettingsTab(contentRect); break;
    default: break;
    }
}

void AcestepAudioProcessorEditor::layoutGenerateTab(juce::Rectangle<int> r)
{
    auto take = [&](int h) { auto row = r.removeFromTop(h); r.removeFromTop(4); return row; };

    // Prompt
    promptLabel_.setVisible(true);
    promptLabel_.setBounds(take(16));
    promptEditor_.setVisible(true);
    promptEditor_.setBounds(take(24));

    // Duration + Steps
    auto row = take(24);
    durationLabel_.setVisible(true); durationLabel_.setBounds(row.removeFromLeft(68));
    durationCombo_.setVisible(true); durationCombo_.setBounds(row.removeFromLeft(70)); row.removeFromLeft(8);
    stepsLabel_.setVisible(true);    stepsLabel_.setBounds(row.removeFromLeft(58));
    stepsCombo_.setVisible(true);    stepsCombo_.setBounds(row);

    // BPM
    row = take(24);
    bpmLabel_.setVisible(true); bpmLabel_.setBounds(row.removeFromLeft(40));
    bpmEditor_.setVisible(true); bpmEditor_.setBounds(row.removeFromLeft(80));

    // Mode
    row = take(26);
    genModeButton_.setVisible(true);   genModeButton_.setBounds(row.removeFromLeft(130)); row.removeFromLeft(4);
    coverModeButton_.setVisible(true); coverModeButton_.setBounds(row.removeFromLeft(120));

    // Cover-only controls
    if (coverModeActive_)
    {
        row = take(24);
        refFileLabel_.setVisible(true);   refFileLabel_.setBounds(row.removeFromLeft(r.getWidth() - 150)); row.removeFromLeft(4);
        browseRefButton_.setVisible(true); browseRefButton_.setBounds(row.removeFromLeft(70)); row.removeFromLeft(4);
        clearRefButton_.setVisible(true);  clearRefButton_.setBounds(row);

        row = take(26);
        coverStrengthLabel_.setVisible(true); coverStrengthLabel_.setBounds(row.removeFromLeft(120));
        coverStrengthSlider_.setVisible(true); coverStrengthSlider_.setBounds(row);
    }

    // Generate button
    r.removeFromTop(4);
    generateButton_.setVisible(true);
    generateButton_.setBounds(r.removeFromTop(30));
    r.removeFromTop(6);

    // Status
    statusLabel_.setVisible(true);
    statusLabel_.setBounds(r.removeFromTop(60));
}

void AcestepAudioProcessorEditor::layoutLibraryTab(juce::Rectangle<int> r)
{
    // Toolbar
    auto toolbar = r.removeFromTop(24); r.removeFromTop(4);
    refreshLibButton_.setVisible(true); refreshLibButton_.setBounds(toolbar.removeFromLeft(80)); toolbar.removeFromLeft(6);
    importButton_.setVisible(true);     importButton_.setBounds(toolbar.removeFromLeft(110));

    // List
    const int listH = juce::jmax(160, r.getHeight() - 4 - 24 - 4 - 24 - 4 - 24 - 4 - 40);
    libraryList_.setVisible(true); libraryList_.setBounds(r.removeFromTop(listH)); r.removeFromTop(4);

    // Playback row
    auto row = r.removeFromTop(24); r.removeFromTop(4);
    previewButton_.setVisible(true); previewButton_.setBounds(row.removeFromLeft(90)); row.removeFromLeft(4);
    stopButton_.setVisible(true);    stopButton_.setBounds(row.removeFromLeft(70));   row.removeFromLeft(4);
    loopButton_.setVisible(true);    loopButton_.setBounds(row.removeFromLeft(70));   row.removeFromLeft(4);
    deleteButton_.setVisible(true);  deleteButton_.setBounds(row.removeFromLeft(70));

    // Reference + DAW row
    row = r.removeFromTop(24); r.removeFromTop(4);
    useAsRefButton_.setVisible(true);  useAsRefButton_.setBounds(row.removeFromLeft(130)); row.removeFromLeft(4);
    insertDawButton_.setVisible(true); insertDawButton_.setBounds(row.removeFromLeft(120)); row.removeFromLeft(4);
    revealButton_.setVisible(true);    revealButton_.setBounds(row);

    // Hint
    libHintLabel_.setVisible(true); libHintLabel_.setBounds(r.removeFromTop(40));
}

void AcestepAudioProcessorEditor::layoutSettingsTab(juce::Rectangle<int> r)
{
    auto labelRow = [&]() -> juce::Rectangle<int> { auto rw = r.removeFromTop(16); r.removeFromTop(2); return rw; };
    auto fieldRow = [&]() -> juce::Rectangle<int> { auto rw = r.removeFromTop(24); r.removeFromTop(8); return rw; };

    binPathLabel_.setVisible(true);  binPathLabel_.setBounds(labelRow());
    auto row = fieldRow();
    binBrowseButton_.setVisible(true); binBrowseButton_.setBounds(row.removeFromRight(80)); row.removeFromRight(4);
    binPathEditor_.setVisible(true);   binPathEditor_.setBounds(row);

    modelsPathLabel_.setVisible(true); modelsPathLabel_.setBounds(labelRow());
    row = fieldRow();
    modelsBrowseButton_.setVisible(true); modelsBrowseButton_.setBounds(row.removeFromRight(80)); row.removeFromRight(4);
    modelsPathEditor_.setVisible(true);   modelsPathEditor_.setBounds(row);

    outputPathLabel_.setVisible(true); outputPathLabel_.setBounds(labelRow());
    row = fieldRow();
    outputBrowseButton_.setVisible(true); outputBrowseButton_.setBounds(row.removeFromRight(80)); row.removeFromRight(4);
    outputPathEditor_.setVisible(true);   outputPathEditor_.setBounds(row);

    applySettingsButton_.setVisible(true); applySettingsButton_.setBounds(r.removeFromTop(28));
    r.removeFromTop(8);

    settingsInfoLabel_.setVisible(true); settingsInfoLabel_.setBounds(r.removeFromTop(100));
}
