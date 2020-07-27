// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "CommandPalette.h"

#include "CommandPalette.g.cpp"
#include <winrt/Microsoft.Terminal.Settings.h>
#include <locale>
#include <numeric>
#include <til/math.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

using namespace winrt;
using namespace winrt::TerminalApp;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::System;
using namespace winrt::Windows::Foundation;

namespace
{
    struct MatchRange
    {
        size_t start, end;
    };
    std::vector<MatchRange> _matchWordwise(const std::wstring_view& needle,
                                           const std::wstring_view& haystack)
    {
        std::vector<MatchRange> ranges;
        const auto start{ haystack.cbegin() };
        auto it = start;
        auto sit = needle.cbegin();

        auto wordStart{ haystack.cend() };
        const auto valid = [&]() { return it != haystack.cend() && sit != needle.cend(); };
        const auto vend = [&]() {
            if (wordStart != haystack.cend())
            {
                ranges.emplace_back(MatchRange{ gsl::narrow<size_t>(wordStart - start), gsl::narrow<size_t>(it - start) });
                wordStart = haystack.cend();
            }
        };
        while (valid())
        {
            while (valid() && *it == *sit)
            {
                if (wordStart == haystack.cend())
                {
                    // start of matching word
                    wordStart = it;
                }
                ++it;
                ++sit;
            }
            // if we get here, we've encountered a mismatch
            // so, spit out the match length
            vend();
            // advance haystack until next word
            for (; it != haystack.cend() && *it != ' '; ++it)
                ;

            if (it != haystack.cend()) // we found a space, now walk off it
            {
                ++it;
            }

            // advance needle if it's on a run of spaces
            for (; sit != needle.cend() && *sit == ' '; ++sit)
                ;
        }

        if (sit != needle.cend())
        {
            // didn't consume the entire needle
            return {};
        }

        vend();
        return ranges;
    }

    std::vector<MatchRange> _matchPrefix(const std::wstring_view& needle,
                                         const std::wstring_view& haystack)
    {
        if (needle.size() > haystack.size())
        {
            return {};
        }

        const auto checkLength{ needle.size() };
        if (haystack.compare(0u, checkLength, needle, 0u, checkLength) == 0)
        {
            return { MatchRange{ 0, checkLength } };
        }
        return {};
    }

    std::vector<MatchRange> _matchContiguousSubstring(const std::wstring_view& needle, const std::wstring_view& haystack)
    {
        if (needle.size() > haystack.size())
        {
            return {};
        }

        const auto found{ haystack.find(needle) };
        if (found != std::wstring::npos)
        {
            return { MatchRange{ found, found + needle.size() } };
        }
        return {};
    }

    std::vector<MatchRange> _firstOrBestMatchRange(const std::wstring_view& needle, const std::wstring_view& haystack)
    {
        std::locale user("");
        std::wstring nl{ needle };
        std::wstring hl{ haystack };
        std::for_each(nl.begin(), nl.end(), [&](auto&& c) { c = std::tolower(c, user); });
        std::for_each(hl.begin(), hl.end(), [&](auto&& c) { c = std::tolower(c, user); });
        if (auto matches{ _matchPrefix(nl, hl) }; matches.size() > 0)
        {
            return matches;
        }
        if (auto matches{ _matchWordwise(nl, hl) }; matches.size() > 0)
        {
            return matches;
        }
        if (auto matches{ _matchContiguousSubstring(nl, hl) }; matches.size() > 0)
        {
            return matches;
        }
        return {};
    }
}

template<>
struct ::fmt::formatter<MatchRange> : fmt::formatter<string_view>
{
    template<typename FormatContext>
    auto format(const MatchRange& mr, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{{{0}-{1}}}", mr.start, mr.end);
    }
};

namespace winrt::TerminalApp::implementation
{
    struct CommandListEntry : CommandListEntryT<CommandListEntry>
    {
        CommandListEntry(const TerminalApp::Command& command) :
            command(command), coverage{ 0 } {}
        CommandListEntry(const TerminalApp::Command& command, const std::vector<MatchRange>& matches) :
            command(command), matches(matches)
        {
            const auto totalMatchLen{ std::accumulate(matches.cbegin(),
                                                      matches.cend(),
                                                      size_t{ 0 },
                                                      [](const size_t val, const MatchRange& mr) { return val + mr.end - mr.start; }) };
            coverage = til::math::rounding.cast<int>((gsl::narrow<double>(totalMatchLen) / command.Name().size()) * 100);
        }
        winrt::hstring Name() const { return command.Name(); }
        winrt::hstring KeyChordText() const { return command.KeyChordText(); }
        Windows::UI::Xaml::Controls::TextBlock RenderedText()
        {
            Windows::UI::Xaml::Controls::TextBlock tb;
            if (matches.size() == 0)
            {
                tb.Text(Name());
                return tb;
            }

            size_t last{ 0 };
            std::wstring_view name{ command.Name() };
            for (const auto& match : matches)
            {
                // vend an unbolded region
                if (last != match.start)
                {
                    Windows::UI::Xaml::Documents::Run r;
                    r.Text(std::wstring{ name.substr(last, match.start - last) });
                    tb.Inlines().Append(r);
                }

                Windows::UI::Xaml::Documents::Run r;
                r.Text(std::wstring{ name.substr(match.start, match.end - match.start) });
                r.FontWeight(Windows::UI::Text::FontWeights::Bold());
                tb.Inlines().Append(r);
                last = match.end;
            }

            if (last != name.size())
            {
                Windows::UI::Xaml::Documents::Run r;
                r.Text(std::wstring{ name.substr(last, std::wstring::npos) });
                tb.Inlines().Append(r);
            }

            std::vector<size_t> starts, ends;
            starts.emplace_back(0);
            for (const auto& match : matches)
            {
                ends.emplace_back(match.start);
                starts.emplace_back(match.start);
                ends.emplace_back(match.end);
                starts.emplace_back(match.end);
            }
            ends.emplace_back(name.size());

            {
                tb.Inlines().Append(Windows::UI::Xaml::Documents::LineBreak{});
                Windows::UI::Xaml::Documents::Run r;
                r.Text(fmt::format(L"Haystack Needle Coverage: {0}%", coverage));
                r.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
                r.FontSize((8 * 96) / 72);
                tb.Inlines().Append(r);
            }
            {
                tb.Inlines().Append(Windows::UI::Xaml::Documents::LineBreak{});
                Windows::UI::Xaml::Documents::Run r;
                r.Text(til::u8u16(fmt::format("ranges: {0} || {1}", fmt::join(starts, ","), fmt::join(ends, ","))));
                r.FontStyle(winrt::Windows::UI::Text::FontStyle::Italic);
                r.FontSize((8 * 96) / 72);
                tb.Inlines().Append(r);
            }
            return tb;
        };

        TerminalApp::Command command;
        std::vector<MatchRange> matches;
        int coverage;
    };

    CommandPalette::CommandPalette()
    {
        InitializeComponent();

        _filteredActions = winrt::single_threaded_observable_vector<winrt::TerminalApp::CommandListEntry>();
        _allActions = winrt::single_threaded_vector<winrt::TerminalApp::Command>();

        if (CommandPaletteShadow())
        {
            // Hook up the shadow on the command palette to the backdrop that
            // will actually show it. This needs to be done at runtime, and only
            // if the shadow actually exists. ThemeShadow isn't supported below
            // version 18362.
            CommandPaletteShadow().Receivers().Append(_shadowBackdrop());
            // "raise" the command palette up by 16 units, so it will cast a shadow.
            _backdrop().Translation({ 0, 0, 16 });
        }

        // Whatever is hosting us will enable us by setting our visibility to
        // "Visible". When that happens, set focus to our search box.
        RegisterPropertyChangedCallback(UIElement::VisibilityProperty(), [this](auto&&, auto&&) {
            if (Visibility() == Visibility::Visible)
            {
                _searchBox().Focus(FocusState::Programmatic);
                _filteredActionsView().SelectedIndex(0);

                TraceLoggingWrite(
                    g_hTerminalAppProvider, // handle to TerminalApp tracelogging provider
                    "CommandPaletteOpened",
                    TraceLoggingDescription("Event emitted when the Command Palette is opened"),
                    TraceLoggingWideString(L"Action", "Mode", "which mode the palette was opened in"),
                    TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                    TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance));
            }
            else
            {
                // Raise an event to return control to the Terminal.
                _dismissPalette();
            }
        });
    }

    // Method Description:
    // - Moves the focus up or down the list of commands. If we're at the top,
    //   we'll loop around to the bottom, and vice-versa.
    // Arguments:
    // - moveDown: if true, we're attempting to move to the next item in the
    //   list. Otherwise, we're attempting to move to the previous.
    // Return Value:
    // - <none>
    void CommandPalette::_selectNextItem(const bool moveDown)
    {
        const auto selected = _filteredActionsView().SelectedIndex();
        const int numItems = ::base::saturated_cast<int>(_filteredActionsView().Items().Size());
        // Wraparound math. By adding numItems and then calculating modulo numItems,
        // we clamp the values to the range [0, numItems) while still supporting moving
        // upward from 0 to numItems - 1.
        const auto newIndex = ((numItems + selected + (moveDown ? 1 : -1)) % numItems);
        _filteredActionsView().SelectedIndex(newIndex);
        _filteredActionsView().ScrollIntoView(_filteredActionsView().SelectedItem());
    }

    // Method Description:
    // - Process keystrokes in the input box. This is used for moving focus up
    //   and down the list of commands in Action mode, and for executing
    //   commands in both Action mode and Commandline mode.
    // Arguments:
    // - e: the KeyRoutedEventArgs containing info about the keystroke.
    // Return Value:
    // - <none>
    void CommandPalette::_keyDownHandler(IInspectable const& /*sender*/,
                                         Windows::UI::Xaml::Input::KeyRoutedEventArgs const& e)
    {
        auto key = e.OriginalKey();

        if (key == VirtualKey::Up)
        {
            // Action Mode: Move focus to the next item in the list.
            _selectNextItem(false);
            e.Handled(true);
        }
        else if (key == VirtualKey::Down)
        {
            // Action Mode: Move focus to the previous item in the list.
            _selectNextItem(true);
            e.Handled(true);
        }
        else if (key == VirtualKey::Enter)
        {
            // Action Mode: Dispatch the action of the selected command.

            if (const auto selectedItem = _filteredActionsView().SelectedItem())
            {
                if (const auto selectedCommand{ selectedItem.try_as<TerminalApp::CommandListEntry>() })
                {
                    auto cle{ winrt::get_self<implementation::CommandListEntry>(selectedCommand) };
                    _dispatchCommand(cle->command);
                }
            }

            e.Handled(true);
        }
        else if (key == VirtualKey::Escape)
        {
            // Action Mode: Dismiss the palette if the text is empty, otherwise clear the search string.
            if (_searchBox().Text().empty())
            {
                _dismissPalette();
            }
            else
            {
                _searchBox().Text(L"");
            }

            e.Handled(true);
        }
    }

    // Method Description:
    // - This event is triggered when someone clicks anywhere in the bounds of
    //   the window that's _not_ the command palette UI. When that happens,
    //   we'll want to dismiss the palette.
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void CommandPalette::_rootPointerPressed(Windows::Foundation::IInspectable const& /*sender*/,
                                             Windows::UI::Xaml::Input::PointerRoutedEventArgs const& /*e*/)
    {
        _dismissPalette();
    }

    // Method Description:
    // - This event is only triggered when someone clicks in the space right
    //   next to the text box in the command palette. We _don't_ want that click
    //   to light dismiss the palette, so we'll mark it handled here.
    // Arguments:
    // - e: the PointerRoutedEventArgs that we want to mark as handled
    // Return Value:
    // - <none>
    void CommandPalette::_backdropPointerPressed(Windows::Foundation::IInspectable const& /*sender*/,
                                                 Windows::UI::Xaml::Input::PointerRoutedEventArgs const& e)
    {
        e.Handled(true);
    }

    // Method Description:
    // - This event is called when the user clicks on an individual item from
    //   the list. We'll get the item that was clicked and dispatch the command
    //   that the user clicked on.
    // Arguments:
    // - e: an ItemClickEventArgs who's ClickedItem() will be the command that was clicked on.
    // Return Value:
    // - <none>
    void CommandPalette::_listItemClicked(Windows::Foundation::IInspectable const& /*sender*/,
                                          Windows::UI::Xaml::Controls::ItemClickEventArgs const& e)
    {
        if (const auto selectedCommand{ e.ClickedItem().try_as<TerminalApp::CommandListEntry>() })
        {
            auto cle{ winrt::get_self<implementation::CommandListEntry>(selectedCommand) };
            _dispatchCommand(cle->command);
        }
    }

    // Method Description:
    // - Helper method for retrieving the action from a command the user
    //   selected, and dispatching that command. Also fires a tracelogging event
    //   indicating that the user successfully found the action they were
    //   looking for.
    // Arguments:
    // - command: the Command to dispatch. This might be null.
    // Return Value:
    // - <none>
    void CommandPalette::_dispatchCommand(const TerminalApp::Command& command)
    {
        if (command)
        {
            const auto actionAndArgs = command.Action();
            _dispatch.DoAction(actionAndArgs);
            _close();

            TraceLoggingWrite(
                g_hTerminalAppProvider, // handle to TerminalApp tracelogging provider
                "CommandPaletteDispatchedAction",
                TraceLoggingDescription("Event emitted when the user selects an action in the Command Palette"),
                TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance));
        }
    }

    // Method Description:
    // - Helper method for closing the command palette, when the user has _not_
    //   selected an action. Also fires a tracelogging event indicating that the
    //   user closed the palette without running a command.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void CommandPalette::_dismissPalette()
    {
        _close();

        TraceLoggingWrite(
            g_hTerminalAppProvider, // handle to TerminalApp tracelogging provider
            "CommandPaletteDismissed",
            TraceLoggingDescription("Event emitted when the user dismisses the Command Palette without selecting an action"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServicePerformance));
    }

    // Method Description:
    // - Event handler for when the text in the input box changes. In Action
    //   Mode, we'll update the list of displayed commands, and select the first one.
    // Arguments:
    // - <unused>
    // Return Value:
    // - <none>
    void CommandPalette::_filterTextChanged(IInspectable const& /*sender*/,
                                            Windows::UI::Xaml::RoutedEventArgs const& /*args*/)
    {
        _updateFilteredActions();
        _filteredActionsView().SelectedIndex(0);

        _noMatchesText().Visibility(_filteredActions.Size() > 0 ? Visibility::Collapsed : Visibility::Visible);
    }

    Collections::IObservableVector<TerminalApp::CommandListEntry> CommandPalette::FilteredActions()
    {
        return _filteredActions;
    }

    void CommandPalette::SetActions(Collections::IVector<TerminalApp::Command> const& actions)
    {
        _allActions = actions;
        _updateFilteredActions();
    }

    // This is a helper to aid in sorting commands by their `Name`s, alphabetically.
    static bool _compareCommandListItemNames(const TerminalApp::CommandListEntry& lhs, const TerminalApp::CommandListEntry& rhs)
    {
        auto leftSelf{ winrt::get_self<implementation::CommandListEntry>(lhs) };
        auto rightSelf{ winrt::get_self<implementation::CommandListEntry>(rhs) };
        std::wstring_view leftName{ lhs.Name() };
        std::wstring_view rightName{ rhs.Name() };
        return leftSelf->coverage != rightSelf->coverage ?
                   leftSelf->coverage > rightSelf->coverage :
                   (leftName.compare(rightName) < 0);
    }

    // Method Description:
    // - Produce a list of filtered actions to reflect the current contents of
    //   the input box. For more details on which commands will be displayed,
    //   see `_getWeight`.
    // Arguments:
    // - A collection that will receive the filtered actions
    // Return Value:
    // - <none>
    std::vector<winrt::TerminalApp::CommandListEntry> CommandPalette::_collectFilteredActions()
    {
        std::vector<winrt::TerminalApp::CommandListEntry> actions;

        auto searchText = _searchBox().Text();
        const bool addAll = searchText.empty();

        // If there's no filter text, then just add all the commands in order to the list.
        // - TODO GH#6647:Possibly add the MRU commands first in order, followed
        //   by the rest of the commands.
        if (addAll)
        {
            // Add all the commands, but make sure they're sorted alphabetically.
            actions.reserve(_allActions.Size());

            for (auto action : _allActions)
            {
                actions.emplace_back(*winrt::make_self<CommandListEntry>(action));
            }
        }
        else
        {
            // Here, there was some filter text.
            // Show these actions in a weighted order.
            // - Matching the first character of a word, then the first char of a
            //   subsequent word seems better than just "the order they appear in
            //   the list".
            // - TODO GH#6647:"Recently used commands" ordering also seems valuable.
            //      * This could be done by weighting the recently used commands
            //        higher the more recently they were used, then weighting all
            //        the unused commands as 1

            // Use a priority queue to order commands so that "better" matches
            // appear first in the list. The ordering will be determined by the
            // match weight produced by _getWeight.
            //std::priority_queue<WeightedCommand> heap;
            for (auto action : _allActions)
            {
                const auto matches{ _firstOrBestMatchRange(searchText, action.Name()) };
                if (matches.size() > 0)
                {
                    actions.emplace_back(*winrt::make_self<implementation::CommandListEntry>(action, matches));
                }
                /*
            const auto weight = CommandPalette::_getWeight(searchText, action.Name());
            if (weight > 0)
            {
                WeightedCommand wc;
                wc.command = action;
                wc.weight = weight;
                heap.push(wc);
            }
            */
            }

            // At this point, all the commands in heap are matches. We've also
            // sorted commands with the same weight alphabetically.
            // Remove everything in-order from the queue, and add to the list of
            // filtered actions.
            /*
        while (!heap.empty())
        {
            auto top = heap.top();
            heap.pop();
            actions.push_back(top.command);
        }
        */
        }

        // finally, sort
        std::sort(actions.begin(),
                  actions.end(),
                  _compareCommandListItemNames);

        return actions;
    }

    // Method Description:
    // - Update our list of filtered actions to reflect the current contents of
    //   the input box. For more details on which commands will be displayed,
    //   see `_getWeight`.
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void CommandPalette::_updateFilteredActions()
    {
        auto actions = _collectFilteredActions();

        // Make _filteredActions look identical to actions, using only Insert and Remove.
        // This allows WinUI to nicely animate the ListView as it changes.
        for (uint32_t i = 0; i < _filteredActions.Size() && i < actions.size(); i++)
        {
            for (uint32_t j = i; j < _filteredActions.Size(); j++)
            {
                if (_filteredActions.GetAt(j) == actions[i])
                {
                    for (uint32_t k = i; k < j; k++)
                    {
                        _filteredActions.RemoveAt(i);
                    }
                    break;
                }
            }

            if (_filteredActions.GetAt(i) != actions[i])
            {
                _filteredActions.InsertAt(i, actions[i]);
            }
        }

        // Remove any extra trailing items from the destination
        while (_filteredActions.Size() > actions.size())
        {
            _filteredActions.RemoveAtEnd();
        }

        // Add any extra trailing items from the source
        while (_filteredActions.Size() < actions.size())
        {
            _filteredActions.Append(actions[_filteredActions.Size()]);
        }
    }

// Function Description:
// - Calculates a "weighting" by which should be used to order a command
//   name relative to other names, given a specific search string.
//   Currently, this is based off of two factors:
//   * The weight is incremented once for each matched character of the
//     search text.
//   * If a matching character from the search text was found at the start
//     of a word in the name, then we increment the weight again.
//     * For example, for a search string "sp", we want "Split Pane" to
//       appear in the list before "Close Pane"
//   * Consecutive matches will be weighted higher than matches with
//     characters in between the search characters.
// - This will return 0 if the command should not be shown. If all the
//   characters of search text appear in order in `name`, then this function
//   will return a positive number. There can be any number of characters
//   separating consecutive characters in searchText.
//   * For example:
//      "name": "New Tab"
//      "name": "Close Tab"
//      "name": "Close Pane"
//      "name": "[-] Split Horizontal"
//      "name": "[ | ] Split Vertical"
//      "name": "Next Tab"
//      "name": "Prev Tab"
//      "name": "Open Settings"
//      "name": "Open Media Controls"
//   * "open" should return both "**Open** Settings" and "**Open** Media Controls".
//   * "Tab" would return "New **Tab**", "Close **Tab**", "Next **Tab**" and "Prev
//     **Tab**".
//   * "P" would return "Close **P**ane", "[-] S**p**lit Horizontal", "[ | ]
//     S**p**lit Vertical", "**P**rev Tab", "O**p**en Settings" and "O**p**en Media
//     Controls".
//   * "sv" would return "[ | ] Split Vertical" (by matching the **S** in
//     "Split", then the **V** in "Vertical").
// Arguments:
// - searchText: the string of text to search for in `name`
// - name: the name to check
// Return Value:
// - the relative weight of this match
#if 0
    int CommandPalette::_getWeight(const winrt::hstring& searchText,
                                   const winrt::hstring& name)
    {
        int totalWeight = 0;
        bool lastWasSpace = true;

        auto it = name.cbegin();

        for (auto searchChar : searchText)
        {
            searchChar = std::towlower(searchChar);
            // Advance the iterator to the next character that we're looking
            // for.

            bool lastWasMatch = true;
            while (true)
            {
                // If we are at the end of the name string, we haven't found
                // it.
                if (it == name.cend())
                {
                    return false;
                }

                // found it
                if (std::towlower(*it) == searchChar)
                {
                    break;
                }

                lastWasSpace = *it == L' ';
                ++it;
                lastWasMatch = false;
            }

            // Advance the iterator by one character so that we don't
            // end up on the same character in the next iteration.
            ++it;

            totalWeight += 1;
            totalWeight += lastWasSpace ? 1 : 0;
            totalWeight += (lastWasMatch) ? 1 : 0;
        }

        return totalWeight;
    }
#endif

    void CommandPalette::SetDispatch(const winrt::TerminalApp::ShortcutActionDispatch& dispatch)
    {
        _dispatch = dispatch;
    }

    // Method Description:
    // - Dismiss the command palette. This will:
    //   * select all the current text in the input box
    //   * set our visibility to Hidden
    //   * raise our Closed event, so the page can return focus to the active Terminal
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void CommandPalette::_close()
    {
        Visibility(Visibility::Collapsed);

        // Clear the text box each time we close the dialog. This is consistent with VsCode.
        _searchBox().Text(L"");
    }
}
