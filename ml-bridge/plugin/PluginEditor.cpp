#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <juce_core/juce_core.h>

// --- LibraryListModel ---
int LibraryListModel::getNumRows()
{
    return static_cast<int>(processor.getLibraryEntries().size());
}

void LibraryListModel::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected)
{
    auto entries = processor.getLibraryEntries();
    if (rowNumber < 0 || rowNumber >= static_cast<int>(entries.size()))
        return;
    const auto& e = entries[static_cast<size_t>(rowNumber)];
    if (rowIsSelected)
        g.fillAll(juce::Colour(0xff2a2a4e));
    g.setColour(juce::Colours::white);
    g.setFont(14.0f);
    g.drawText(e.file.getFileName(), 6, 0, width - 12, height, juce::Justification::centredLeft);
    g.setColour(juce::Colours::lightgrey);
    g.setFont(11.0f);
    g.drawText(e.time.formatted("%Y-%m-%d %H:%M"), 6, 0, width - 12, height, juce::Justification::centredRight);
}

void LibraryListModel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (onRowDoubleClicked_)
        onRowDoubleClicked_(row);
}

// --- LibraryListBox ---
LibraryListBox::LibraryListBox(AceForgeBridgeAudioProcessor& p, LibraryListModel& model)
    : ListBox("Library", &model), processorRef(p)
{
    setRowHeight(28);
    setOutlineThickness(0);
}

void LibraryListBox::mouseDrag(const juce::MouseEvent& e)
{
    if (dragStarted_)
    {
        ListBox::mouseDrag(e);
        return;
    }
    if (e.getDistanceFromDragStart() < 10)
    {
        ListBox::mouseDrag(e);
        return;
    }
    int row = getRowContainingPosition(e.x, e.y);
    auto entries = processorRef.getLibraryEntries();
    if (row < 0 || row >= static_cast<int>(entries.size()))
    {
        ListBox::mouseDrag(e);
        return;
    }
    juce::String path = entries[static_cast<size_t>(row)].file.getFullPathName();
    if (path.isEmpty())
    {
        ListBox::mouseDrag(e);
        return;
    }
    auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this);
    // Use the editor (top-level plugin view) as drag source so the host can accept the drop
    auto* editorComp = findParentComponentOfClass<AceForgeBridgeAudioProcessorEditor>();
    juce::Component* sourceComp = editorComp != nullptr ? static_cast<juce::Component*>(editorComp) : static_cast<juce::Component*>(this);
    if (container && container->performExternalDragDropOfFiles(juce::StringArray(path), false, sourceComp))
        dragStarted_ = true;
    else
        ListBox::mouseDrag(e);
}

void LibraryListBox::mouseUp(const juce::MouseEvent& e)
{
    dragStarted_ = false;
    ListBox::mouseUp(e);
}

// --- Editor ---
AceForgeBridgeAudioProcessorEditor::AceForgeBridgeAudioProcessorEditor(
    AceForgeBridgeAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p), libraryListModel(p), libraryList(p, libraryListModel)
{
    setSize(460, 500);

    connectionLabel.setText("Engine: checking…", juce::dontSendNotification);
    connectionLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    connectionLabel.setJustificationType(juce::Justification::left);
    addAndMakeVisible(connectionLabel);

    promptEditor.setMultiLine(false);
    promptEditor.setReturnKeyStartsNewLine(false);
    promptEditor.setText("upbeat electronic beat, 10s");
    promptEditor.setTextToShowWhenEmpty("Describe the music (e.g. calm piano, 10s)", juce::Colours::grey);
    addAndMakeVisible(promptEditor);

    durationLabel.setText("Duration (s):", juce::dontSendNotification);
    durationLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(durationLabel);

    durationCombo.addItem("10", 10);
    durationCombo.addItem("15", 15);
    durationCombo.addItem("20", 20);
    durationCombo.addItem("30", 30);
    durationCombo.setSelectedId(10, juce::dontSendNotification);
    addAndMakeVisible(durationCombo);

    qualityLabel.setText("Quality:", juce::dontSendNotification);
    qualityLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(qualityLabel);

    qualityCombo.addItem("Fast (15 steps)", 15);
    qualityCombo.addItem("High (55 steps)", 55);
    qualityCombo.setSelectedId(15, juce::dontSendNotification);
    addAndMakeVisible(qualityCombo);

    generateButton.setButtonText("Generate");
    generateButton.onClick = [this] { startGeneration(); };
    addAndMakeVisible(generateButton);

    statusLabel.setText("Idle - enter a prompt and click Generate.", juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    statusLabel.setJustificationType(juce::Justification::topLeft);
    statusLabel.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(statusLabel);

    libraryLabel.setText("Library", juce::dontSendNotification);
    libraryLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(libraryLabel);

    refreshLibraryButton.setButtonText("Refresh");
    refreshLibraryButton.onClick = [this] { refreshLibraryList(); };
    addAndMakeVisible(refreshLibraryButton);

    addAndMakeVisible(libraryList);

    insertIntoDawButton.setButtonText("Insert into DAW");
    insertIntoDawButton.onClick = [this] { insertSelectedIntoDaw(); };
    addAndMakeVisible(insertIntoDawButton);

    revealInFinderButton.setButtonText("Reveal in Finder");
    revealInFinderButton.onClick = [this] { revealSelectedInFinder(); };
    addAndMakeVisible(revealInFinderButton);

    libraryHintLabel.setText("Select a row, click Insert into DAW (opens in Logic Pro) or Reveal in Finder and drag the file onto the timeline.", juce::dontSendNotification);
    libraryHintLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    libraryHintLabel.setFont(juce::Font(juce::FontOptions().withPointHeight(10.0f)));
    libraryHintLabel.setMinimumHorizontalScale(1.0f);
    addAndMakeVisible(libraryHintLabel);

    libraryListModel.setOnRowDoubleClicked([this](int row) {
        auto entries = processorRef.getLibraryEntries();
        if (row < 0 || row >= static_cast<int>(entries.size()))
            return;
        juce::SystemClipboard::copyTextToClipboard(entries[static_cast<size_t>(row)].file.getFullPathName());
        showLibraryFeedback();
    });

    startTimerHz(4);
}

AceForgeBridgeAudioProcessorEditor::~AceForgeBridgeAudioProcessorEditor()
{
    stopTimer();
}

void AceForgeBridgeAudioProcessorEditor::timerCallback()
{
    if (libraryFeedbackCountdown_ > 0)
    {
        statusLabel.setText(libraryFeedbackMessage_, juce::dontSendNotification);
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgreen);
        --libraryFeedbackCountdown_;
    }
    else
    {
        updateStatusFromProcessor();
    }
    libraryList.updateContent();
}

void AceForgeBridgeAudioProcessorEditor::updateStatusFromProcessor()
{
    const auto state = processorRef.getState();
    if (state == AceForgeBridgeAudioProcessor::State::Succeeded)
        refreshLibraryList();

    if (processorRef.areBinariesReady())
        connectionLabel.setText("Engine: ready", juce::dontSendNotification);
    else if (state == AceForgeBridgeAudioProcessor::State::Failed)
        connectionLabel.setText("Engine: error (see status)", juce::dontSendNotification);
    else
        connectionLabel.setText("Engine: binaries not found — build acestep.cpp first", juce::dontSendNotification);

    statusLabel.setText(processorRef.getStatusText(), juce::dontSendNotification);
    if (state == AceForgeBridgeAudioProcessor::State::Failed)
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::salmon);
    else
        statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

    const bool busy = (state == AceForgeBridgeAudioProcessor::State::Submitting ||
                      state == AceForgeBridgeAudioProcessor::State::Running);
    generateButton.setEnabled(!busy);
}

void AceForgeBridgeAudioProcessorEditor::startGeneration()
{
    const int durationSec = durationCombo.getSelectedId();
    const int steps = qualityCombo.getSelectedId();
    processorRef.startGeneration(promptEditor.getText(), durationSec > 0 ? durationSec : 10, steps > 0 ? steps : 15);
}

void AceForgeBridgeAudioProcessorEditor::refreshLibraryList()
{
    libraryList.updateContent();
    libraryList.repaint();
}

void AceForgeBridgeAudioProcessorEditor::insertSelectedIntoDaw()
{
    const int row = libraryList.getSelectedRow();
    auto entries = processorRef.getLibraryEntries();
    if (row < 0 || row >= static_cast<int>(entries.size()))
    {
        libraryFeedbackMessage_ = "Select a library entry first.";
        libraryFeedbackCountdown_ = 8;
        return;
    }
    const juce::File& file = entries[static_cast<size_t>(row)].file;
    if (!file.existsAsFile())
    {
        libraryFeedbackMessage_ = "File not found.";
        libraryFeedbackCountdown_ = 8;
        return;
    }
    juce::String path = file.getFullPathName();
    juce::SystemClipboard::copyTextToClipboard(path);

#if JUCE_MAC
    // Open the file with Logic Pro (opens in a new project with the audio; user can drag into main project)
    juce::ChildProcess proc;
    juce::StringArray args;
    args.add("open");
    args.add("-a");
    args.add("Logic Pro");
    args.add(path);
    if (proc.start(args, 0))
        libraryFeedbackMessage_ = "Opened in Logic Pro. Drag the audio from that project into your main project, or use Reveal in Finder.";
    else
        libraryFeedbackMessage_ = "Path copied. Use Reveal in Finder and drag the file into Logic.";
#else
    libraryFeedbackMessage_ = "Path copied to clipboard.";
#endif
    libraryFeedbackCountdown_ = 14;
}

void AceForgeBridgeAudioProcessorEditor::revealSelectedInFinder()
{
    const int row = libraryList.getSelectedRow();
    auto entries = processorRef.getLibraryEntries();
    if (row < 0 || row >= static_cast<int>(entries.size()))
    {
        libraryFeedbackMessage_ = "Select a library entry first.";
        libraryFeedbackCountdown_ = 8;
        return;
    }
    const juce::File& f = entries[static_cast<size_t>(row)].file;
    if (f.existsAsFile())
        f.revealToUser();
    else
    {
        libraryFeedbackMessage_ = "File not found.";
        libraryFeedbackCountdown_ = 8;
    }
}

void AceForgeBridgeAudioProcessorEditor::showLibraryFeedback()
{
    libraryFeedbackMessage_ = "Path copied. Click Insert into DAW to open in Logic, or Reveal in Finder and drag.";
    libraryFeedbackCountdown_ = 12;
}

void AceForgeBridgeAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1a1a2e));
    g.setColour(juce::Colours::white);
    g.setFont(18.0f);
    auto r = getLocalBounds().reduced(12);
    g.drawText("AceForge-Bridge", r.getX(), r.getY(), 220, 26, juce::Justification::left);
}

void AceForgeBridgeAudioProcessorEditor::resized()
{
    const int pad = 12;
    auto r = getLocalBounds().reduced(pad);
    r.removeFromTop(26);

    connectionLabel.setBounds(r.getX(), r.getY(), r.getWidth(), 22);
    r.removeFromTop(22);
    r.removeFromTop(6);

    auto row = r.removeFromTop(24);
    promptEditor.setBounds(row.getX(), row.getY(), row.getWidth(), 24);
    r.removeFromTop(6);

    row = r.removeFromTop(24);
    durationLabel.setBounds(row.getX(), row.getY(), 80, 22);
    durationCombo.setBounds(row.getX() + 82, row.getY(), 56, 22);
    qualityLabel.setBounds(row.getX() + 146, row.getY(), 52, 22);
    qualityCombo.setBounds(row.getX() + 200, row.getY(), 120, 22);
    generateButton.setBounds(row.getX() + 324, row.getY(), 100, 22);
    r.removeFromTop(8);

    statusLabel.setBounds(r.getX(), r.getY(), r.getWidth(), 44);
    r.removeFromTop(44);

    auto libHeader = r.removeFromTop(22);
    libraryLabel.setBounds(libHeader.getX(), libHeader.getY(), 60, 22);
    refreshLibraryButton.setBounds(libHeader.getX() + 64, libHeader.getY(), 60, 22);
    r.removeFromTop(4);

    libraryList.setBounds(r.getX(), r.getY(), r.getWidth(), 120);
    r.removeFromTop(120);
    r.removeFromTop(4);

    auto btnRow = r.removeFromTop(24);
    insertIntoDawButton.setBounds(btnRow.getX(), btnRow.getY(), 120, 22);
    revealInFinderButton.setBounds(btnRow.getX() + 124, btnRow.getY(), 110, 22);
    r.removeFromTop(4);

    libraryHintLabel.setBounds(r.getX(), r.getY(), r.getWidth(), 32);
}
