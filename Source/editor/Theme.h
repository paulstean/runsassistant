#pragma once

// Editor theme (plan.md P3): flat dark look matching the Eloquent house
// style (specification.md D16 / S9 "Eloquent palette"). Colors mirror
// Eloquent's Theme.h; widgets are borderless flat fills, keyboard focus
// draws the only outline. ASCII-only strings everywhere.

#include <juce_gui_basics/juce_gui_basics.h>

namespace runui
{
inline juce::Colour bg()          { return juce::Colour (0xff14161a); }
inline juce::Colour panel()       { return juce::Colour (0xff1c1f25); }
inline juce::Colour edge()        { return juce::Colour (0xff2c313a); }
inline juce::Colour fieldBg()     { return juce::Colour (0xff101317); }
inline juce::Colour text()        { return juce::Colour (0xffc9ced6); }
inline juce::Colour dim()         { return juce::Colour (0xff8b93a0); }
inline juce::Colour accent()      { return juce::Colour (0xff61afef); }
inline juce::Colour btnBg()       { return juce::Colour (0xff22262e); }
inline juce::Colour btnHover()    { return juce::Colour (0xff2a3038); }
inline juce::Colour menuBg()      { return juce::Colour (0xff1a1d23); }
inline juce::Colour warn()        { return juce::Colour (0xffe06c75); }

inline juce::Font font (float size)
{
    return juce::Font (juce::FontOptions (size));
}

// Component property used by RunsLookAndFeel::drawToggleButton to pick the
// indicator shape: 0 = plain flat text button (engine switches), 1 =
// checkbox (pitch-class grid, settings dialog), 2 = radio indicator
// (Walk). Defaults to 0.
inline void setToggleShape (juce::ToggleButton& b, int shape)
{
    b.getProperties().set ("toggleShape", shape);
}

inline int toggleShape (const juce::ToggleButton& b)
{
    return b.getProperties().getWithDefault ("toggleShape", 0);
}

class RunsLookAndFeel : public juce::LookAndFeel_V4
{
public:
    RunsLookAndFeel()
    {
        setColour (juce::TextButton::buttonColourId, btnBg());
        setColour (juce::TextButton::textColourOffId, text());
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        setColour (juce::ToggleButton::textColourId, text());
        setColour (juce::ToggleButton::tickColourId, accent());
        setColour (juce::ToggleButton::tickDisabledColourId, dim());
        setColour (juce::ComboBox::backgroundColourId, btnBg());
        setColour (juce::ComboBox::textColourId, text());
        setColour (juce::ComboBox::arrowColourId, dim());
        setColour (juce::ComboBox::outlineColourId, edge());
        setColour (juce::ComboBox::buttonColourId, btnBg());
        setColour (juce::TextEditor::backgroundColourId, fieldBg());
        setColour (juce::TextEditor::textColourId, text());
        setColour (juce::TextEditor::highlightColourId, accent());
        setColour (juce::CaretComponent::caretColourId, text());
        setColour (juce::Label::textColourId, dim());
        setColour (juce::Label::backgroundColourId, fieldBg());
        setColour (juce::Label::textWhenEditingColourId, text());
        setColour (juce::Label::backgroundWhenEditingColourId, fieldBg());
        setColour (juce::PopupMenu::backgroundColourId, menuBg());
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent());
        setColour (juce::PopupMenu::textColourId, text());
        setColour (juce::Slider::backgroundColourId, fieldBg());
        setColour (juce::Slider::trackColourId, accent());
        setColour (juce::Slider::thumbColourId, dim());
        setColour (juce::Slider::textBoxTextColourId, text());
        setColour (juce::Slider::textBoxBackgroundColourId, fieldBg());
        setColour (juce::TooltipWindow::backgroundColourId,
                   juce::Colours::black);
        setColour (juce::TooltipWindow::textColourId, text());
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool highlighted, bool down) override
    {
        auto r = button.getLocalBounds();
        juce::Colour fill = backgroundColour;
        if (button.getToggleState() || down)
            fill = accent();
        else if (highlighted)
            fill = btnHover();
        g.setColour (button.isEnabled() ? fill : fill.withAlpha (0.5f));
        g.fillRect (r);
        if (button.hasKeyboardFocus (false))
        {
            g.setColour (accent());
            g.drawRect (r.reduced (2), 1);
        }
    }

    juce::Font getTextButtonFont (juce::TextButton&, int) override
    {
        return font (13.0f);
    }

    juce::Font getLabelFont (juce::Label&) override { return font (13.0f); }

    juce::Font getComboBoxFont (juce::ComboBox&) override
    {
        return font (13.0f);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b,
                           bool highlighted, bool down) override
    {
        auto r = b.getLocalBounds();
        const bool on = b.getToggleState();
        juce::Colour fill = btnBg();
        if (on || down)
            fill = accent();
        else if (highlighted)
            fill = btnHover();
        g.setColour (b.isEnabled() ? fill : fill.withAlpha (0.5f));
        g.fillRect (r);
        g.setFont (font (13.0f));

        const int shape = toggleShape (b);
        auto labelArea = r;
        if (shape == 2) // radio dot (Walk)
        {
            auto dot = juce::Rectangle<float> (r.toFloat());
            dot = dot.withX (dot.getX() + 6.0f);
            const float d = juce::jmin (12.0f, r.getHeight() - 8.0f);
            auto circ = juce::Rectangle<float> (d, d).withY (
                (r.getHeight() - d) * 0.5f);
            g.setColour (edge());
            g.fillEllipse (circ);
            if (on)
            {
                g.setColour (juce::Colours::white);
                g.fillEllipse (circ.expanded (-3.0f));
            }
            labelArea = r.withTrimmedLeft ((int) (6.0f + d + 6.0f));
        }
        else if (shape == 1) // checkbox square (pitch-class grid etc.)
        {
            const float side = juce::jmin (12.0f, r.getHeight() - 8.0f);
            auto sq = juce::Rectangle<float> (side, side)
                          .withX (r.toFloat().getX() + 6.0f)
                          .withY ((r.getHeight() - side) * 0.5f);
            g.setColour (on ? accent() : edge());
            g.fillRect (sq);
            if (on)
            {
                g.setColour (juce::Colours::white);
                g.drawLine ({ sq.getX() + 2.0f, sq.getY() + 2.0f,
                              sq.getBottom() - 2.0f, sq.getRight() - 2.0f },
                            2.0f);
                g.drawLine ({ sq.getRight() - 2.0f, sq.getY() + 2.0f,
                              sq.getX() + 2.0f, sq.getBottom() - 2.0f }, 2.0f);
            }
            labelArea = r.withTrimmedLeft ((int) (6.0f + side + 6.0f));
        }
        g.setColour (b.isEnabled()
                         ? (on ? juce::Colours::white : text())
                         : dim().withAlpha (0.5f));
        g.drawText (b.getButtonText(), labelArea.reduced (6, 0),
                    juce::Justification::centredLeft);
        if (b.hasKeyboardFocus (false))
        {
            g.setColour (accent());
            g.drawRect (r.reduced (2), 1);
        }
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width,
                           int height, float sliderPos, float minSliderPos,
                           float maxSliderPos,
                           const juce::Slider::SliderStyle style,
                           juce::Slider&) override
    {
        const bool horiz = style == juce::Slider::LinearHorizontal
                           || style == juce::Slider::LinearBar;
        g.setColour (fieldBg());
        juce::Rectangle<float> track;
        if (horiz)
            track = { (float) x, (float) y + (float) height * 0.5f - 2.0f,
                      (float) width, 4.0f };
        else
            track = { (float) x + (float) width * 0.5f - 2.0f, (float) y,
                      4.0f, (float) height };
        g.fillRect (track);
        g.setColour (accent());
        if (horiz)
            g.fillRect (track.withWidth (
                juce::jmax (0.0f, sliderPos - track.getX())));
        else
            g.fillRect (track.withTop (sliderPos));
        const float side = 12.0f;
        juce::Rectangle<float> thumb;
        if (horiz)
            thumb = juce::Rectangle<float> (side, side)
                        .withCentre ({ sliderPos, track.getCentreY() });
        else
            thumb = juce::Rectangle<float> (side, side)
                        .withCentre ({ track.getCentreX(), sliderPos });
        g.setColour (dim());
        g.fillRect (thumb);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int,
                       int, int, int, juce::ComboBox& box) override
    {
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRect (0, 0, width, height);
        if (box.hasKeyboardFocus (true))
        {
            g.setColour (accent());
            g.drawRect (0, 0, width, height, 1);
        }
        juce::Path arrow;
        const float cx = (float) width - 12.0f;
        const float cy = (float) height * 0.5f;
        arrow.addTriangle (cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f,
                           cx, cy + 3.0f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId));
        g.fillPath (arrow);
    }

    void drawTextEditorOutline (juce::Graphics& g, int width, int height,
                                juce::TextEditor& editor) override
    {
        if (! editor.isEnabled() && ! editor.hasKeyboardFocus (true))
            return;
        if (editor.hasKeyboardFocus (true))
        {
            g.setColour (accent());
            g.drawRect (0, 0, width, height, 1);
        }
        else
        {
            g.setColour (edge());
            g.drawRect (0, 0, width, height, 1);
        }
    }

    void drawPopupMenuBackground (juce::Graphics& g, int width,
                                  int height) override
    {
        g.setColour (menuBg());
        g.fillRect (0, 0, width, height);
        g.setColour (edge());
        g.drawRect (0, 0, width, height, 1);
    }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                            bool isSeparator, bool isActive, bool isHigh,
                            bool isTicked, bool, const juce::String& text1,
                            const juce::String& shortcut, const juce::Drawable*,
                            const juce::Colour*) override
    {
        if (isSeparator)
        {
            g.setColour (edge());
            g.fillRect (area.getX(), area.getY() + area.getHeight() / 2,
                        area.getWidth(), 1);
            return;
        }
        if (isHigh && isActive)
        {
            g.setColour (accent());
            g.fillRect (area);
        }
        g.setFont (font (13.0f));
        g.setColour (! isActive ? dim()
                                 : isHigh ? juce::Colours::white : text());
        auto r = area.reduced (8, 0);
        if (shortcut.isNotEmpty())
        {
            auto rs = r.removeFromRight (40);
            g.drawText (shortcut, rs, juce::Justification::centredRight);
        }
        g.drawText ((isTicked ? "* " : "") + text1, r,
                    juce::Justification::centredLeft);
    }
};
}
