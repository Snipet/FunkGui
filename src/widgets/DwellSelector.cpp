#include <funkgui/widgets/DwellSelector.h>

// DwellSelector (02 §5.7, §7.1): a template, so its members live in the header. ScreenFader (DwellSelector<int>) is
// instantiated here once, for every product that switches screens, and every member is compiled with FunkGui's own
// warnings even before a product uses it.

namespace funkgui
{
    template class DwellSelector<int>;
}
