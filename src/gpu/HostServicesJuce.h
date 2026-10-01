#pragma once

// EditorHost's side of the services of panel/HostServices.h (Web Sprint B, v0.12.0; FCompressor ADR-93): a popup menu,
// a file chooser and the clipboard over JUCE, done as FCompressor's views did them with JUCE themselves, so a view that
// swaps its own code for the HostServices call changes nothing a user sees.
//
// Private to src/gpu: EditorHost owns one for its whole life and forwards to it. Message thread only.

#include <funkgui/panel/HostServices.h>

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace funkgui
{
    class MenuLook;

    class HostServicesJuce
    {
    public:
        explicit HostServicesJuce(juce::Component& owner);   // the editor: menus anchor to it, choosers parent on it
        ~HostServicesJuce();                                 // letGo()

        HostServicesJuce(const HostServicesJuce&) = delete;
        HostServicesJuce& operator=(const HostServicesJuce&) = delete;

        // HostServices::showMenu. `scale` is editor px per Panel px (the UI zoom).
        bool showMenu(const MenuRequest&, MenuCallback, float scale);
        void dismissMenus();
        bool chooseFiles(const FileRequest&, FilesCallback);
        bool copyText(std::string_view utf8);

        // The host lets go of the Panel (the editor left its window, or is going): the menu is dismissed, the chooser
        // closed, and neither callback runs.
        void letGo();

    private:
        // What a JUCE callback finds when it arrives: shared with it, so one that arrives after this object has gone
        // finds nothing to do.
        struct Pending
        {
            MenuCallback  menu;
            FilesCallback files;
            uint32_t menuSerial = 0, filesSerial = 0;        // of the request each callback belongs to
            bool     menuOpen = false, filesOpen = false;
            bool     asking = false;                         // inside showMenu or chooseFiles: nothing is called back
        };

        juce::Component& owner_;
        std::unique_ptr<MenuLook> look_;                     // made with the first menu, as the views made theirs
        std::unique_ptr<juce::FileChooser> chooser_;
        std::unique_ptr<juce::FileChooser> retired_;         // the finished chooser before chooser_ (see the .cpp)
        std::shared_ptr<Pending> pending_;
    };
}
