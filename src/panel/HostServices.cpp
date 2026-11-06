#include <funkgui/panel/HostServices.h>

// validMenu (0.25.0; panel/HostServices.h): the one rule every host of this library applies before it shows a menu
// (HostServicesJuce, HeadlessHost and WebServices each kept a copy of the flat rule before submenus and headers).

namespace funkgui
{
    namespace
    {
        // 1: the rows hold an item that can be chosen (in them or in a submenu); 0: they hold none; -1: refused (an
        // item that could be chosen has no id, or a submenu holds nothing to choose).
        int scan(const std::vector<MenuItem>& items)
        {
            int found = 0;
            for (const MenuItem& it : items)
            {
                if (it.separator || it.header)
                    continue;
                if (!it.submenu.empty())
                {
                    if (scan(it.submenu) <= 0)
                        return -1;
                    found = 1;
                    continue;
                }
                if (it.id <= 0)
                    return -1;
                found = 1;
            }
            return found;
        }
    }

    bool validMenu(const MenuRequest& request)
    {
        return scan(request.items) == 1;
    }
}
