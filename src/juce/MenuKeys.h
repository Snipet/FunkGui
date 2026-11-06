#pragma once

// MenuKeys (0.25.0): where MenuLook's keys move in a menu (juce/MenuLook.h, "Keys"), as plain functions of the rows, so
// a test can pin them without a menu window. Private to src/juce; fg.menulook includes it. A row is selectable when the
// user can choose it or open its submenu (not a separator, a header or a disabled row). Each returns the row to move
// to, or -1 for none (no selectable row, or no match).

#include <juce_core/juce_core.h>

#include <vector>

namespace funkgui::menukeys
{
    // Up (delta -1) and Down (+1): the next selectable row, wrapping as JUCE's keys do. From no row (current -1), Down
    // goes to the first and Up to the last.
    inline int step(const std::vector<bool>& selectable, int current, int delta)
    {
        const int n = static_cast<int>(selectable.size());
        if (n == 0 || delta == 0)
            return -1;
        int at = current < 0 || current >= n ? (delta > 0 ? -1 : n) : current;
        for (int k = 0; k < n; ++k)
        {
            at = ((at + (delta > 0 ? 1 : -1)) % n + n) % n;
            if (selectable[static_cast<size_t>(at)])
                return at;
        }
        return -1;
    }

    // Home (last false) and End (true): the first or the last selectable row.
    inline int edge(const std::vector<bool>& selectable, bool last)
    {
        const int n = static_cast<int>(selectable.size());
        for (int k = 0; k < n; ++k)
        {
            const int at = last ? n - 1 - k : k;
            if (selectable[static_cast<size_t>(at)])
                return at;
        }
        return -1;
    }

    // Page Down (dir +1) and Page Up (-1): `rows` rows on from the current one (from no row: from before the first or
    // after the last), the selectable row nearest that point on the near side, without wrapping; the edge's row when
    // the page runs past it.
    inline int page(const std::vector<bool>& selectable, int current, int rows, int dir)
    {
        const int n = static_cast<int>(selectable.size());
        if (n == 0)
            return -1;
        if (rows < 1)
            rows = 1;
        const int from = current < 0 || current >= n ? (dir > 0 ? -1 : n) : current;
        int target = from + (dir > 0 ? rows : -rows);
        if (target >= n - 1 || target <= 0)
            return edge(selectable, target >= n - 1);
        for (int at = target; at != from; at += dir > 0 ? -1 : 1)  // back towards the current row
            if (selectable[static_cast<size_t>(at)])
                return at;
        for (int at = target; at >= 0 && at < n; at += dir > 0 ? 1 : -1)   // none between: on past the target
            if (selectable[static_cast<size_t>(at)])
                return at;
        return -1;
    }

    // Type-ahead: the first selectable row whose label, less its leading spaces, starts with `typed` (case aside),
    // searching from the current row when `typed` is longer than one character (it refines the current match) and
    // from the row after it otherwise, wrapping. A run of one character ("bbb") with no row that starts with it all
    // moves on to the next row after the current one that starts with that character.
    inline int typeAhead(const juce::StringArray& labels, const std::vector<bool>& selectable, int current,
                         const juce::String& typed)
    {
        const int n = juce::jmin(labels.size(), static_cast<int>(selectable.size()));
        if (n == 0 || typed.isEmpty())
            return -1;
        if (current >= n)
            current = -1;
        const auto matches = [&](int at, const juce::String& prefix) {
            return selectable[static_cast<size_t>(at)] && labels[at].trimStart().startsWithIgnoreCase(prefix);
        };
        const int start = typed.length() > 1 && current >= 0 ? current : current + 1;
        for (int k = 0; k < n; ++k)
        {
            const int at = (start + k) % n;
            if (matches(at, typed))
                return at;
        }
        const juce::String first = typed.substring(0, 1);
        bool repeated = typed.length() > 1;
        for (int i = 1; i < typed.length() && repeated; ++i)
            repeated = typed.substring(i, i + 1).equalsIgnoreCase(first);
        if (repeated)
        {
            for (int k = 1; k <= n; ++k)
            {
                const int at = (current + k + n) % n;
                if (matches(at, first))
                    return at;
            }
        }
        return -1;
    }
}
