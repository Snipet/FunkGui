#include "HostServicesJuce.h"

#include <funkgui/juce/MenuLook.h>

#include <string>
#include <utility>
#include <vector>

// HostServicesJuce (Web Sprint B; panel/HostServices.h). What FCompressor's EditControls, PresetStrip, PresetBrowser
// and Settings did with JUCE at v1.2, behind HostServices' rules. Choices where the card is silent:
//
// - One MenuLook, made with the first menu and re-themed for each (the views each kept one the same way).
// - 0.25.0: every field of the request reaches JUCE. The rows through MenuLook::popupMenuFor (headers, submenus, whose
//   chosen id JUCE returns from any level, radio items, shortcuts); selectedId as JUCE's initially selected item that
//   must be visible (a top-level one: JUCE looks no deeper); minWidth as JUCE's minimum width, the Panel's px under
//   the zoom plus the look's margins; the anchor through MenuLook::targetAreaFor. The menu has one column, so a long
//   one scrolls (MenuLook).
// - JUCE's scale for the menu (Component::getApproximateScaleFactorForComponent of the owner: a host's content scale
//   applied to the editor as a transform, for one) is the owner's px to the screen's, and JUCE scales the menu's
//   window by it too: the window's px, which JUCE's minimum width is in, are the owner's. So minWidth under the zoom
//   is the panel's width as it is, and only the anchor, which is on the screen, is converted (the margin round it is
//   the window's, so targetAreaFor scales it).
// - JUCE has no handle on one popup menu, only PopupMenu::dismissAllActiveMenus(), which closes every menu of the
//   process and takes this look off them. It is called only while a menu of this host is showing: for dismissMenus(),
//   for a second showMenu and for letGo(). The dismissed menu's own JUCE callback arrives later with 0 and finds its
//   request gone.
// - A JUCE callback runs the Panel's only when its serial is still the request's; the Panel's callback is taken out
//   of Pending first, so it may ask for another menu or chooser. JUCE calls back from the message loop, never from
//   inside showMenuAsync or launchAsync; should a backend ever do so, the call is posted to the message loop instead
//   (HostServices' rule: never from inside the call that took it).
// - The chooser starts in the user's Documents folder. A save starts at the suggested name there, made legal by
//   File::createLegalFileName; when the name ends in the extension the pattern demands, the stem is made legal and
//   the extension put back, which is what the views did (legal(name) + extension: the two differ only for a name
//   longer than JUCE's 128 characters). The chosen path of a save gets that extension by File::hasFileExtension and
//   withFileExtension, as the views applied them.
// - A chooser that is replaced while open, or let go, is destroyed, which closes its native dialog without a callback
//   (JUCE). One that has finished is kept until the chooser after next, so a callback that asks for another chooser
//   never destroys the juce::FileChooser it is being called from.

namespace funkgui
{
    namespace
    {
        juce::String fromUtf8(std::string_view s)
        {
            return juce::String::fromUTF8(s.data(), static_cast<int>(s.size()));
        }

        // The extension a save must end in: ".ext" for a pattern that is a single "*.ext", else "" (no rule).
        juce::String saveExtension(std::string_view pattern)
        {
            if (pattern.size() < 3 || pattern[0] != '*' || pattern[1] != '.')
                return {};
            const std::string_view ext = pattern.substr(1);
            return ext.find_first_of("*?;, ") == std::string_view::npos ? fromUtf8(ext) : juce::String();
        }
    }

    HostServicesJuce::HostServicesJuce(juce::Component& owner) : owner_(owner), pending_(std::make_shared<Pending>()) {}

    HostServicesJuce::~HostServicesJuce()
    {
        letGo();
    }

    bool HostServicesJuce::showMenu(const MenuRequest& request, MenuCallback done, float scale)
    {
        dismissMenus();                              // a second menu replaces the first, even when it is refused
        // HostServices' rule (validMenu). JUCE opens no window for a menu with nothing to choose (PopupMenu drops
        // leading and repeated separators, and deletes the callback of an empty menu unrun), so taking one would leave
        // Pending waiting for a callback that never comes.
        if (!validMenu(request))
            return false;
        if (look_ == nullptr)
            look_ = std::make_unique<MenuLook>(request.theme);
        look_->setTheme(request.theme);

        juce::PopupMenu m = MenuLook::popupMenuFor(request);
        m.setLookAndFeel(look_.get());
        const juce::PopupMenu::Options options = menuOptions(request, *look_, owner_, scale);

        Pending& p = *pending_;
        const uint32_t serial = ++p.menuSerial;
        p.menu = std::move(done);
        p.menuOpen = true;
        const auto finish = [pending = pending_, serial](int id) {
            if (!pending->menuOpen || pending->menuSerial != serial)
                return;                              // dismissed, replaced or let go since
            pending->menuOpen = false;
            const MenuCallback callback = std::exchange(pending->menu, nullptr);
            if (callback)
                callback(id > 0 ? id : 0);
        };
        p.asking = true;
        m.showMenuAsync(options,
                        [pending = pending_, finish](int id) {
                            if (pending->asking)
                                juce::MessageManager::callAsync([finish, id] { finish(id); });
                            else
                                finish(id);
                        });
        p.asking = false;
        return true;
    }

    juce::PopupMenu::Options HostServicesJuce::menuOptions(const MenuRequest& request, const MenuLook& look,
                                                           juce::Component& owner, float scale)
    {
        // The Panel's logical px to the owner's (the UI zoom), then to the screen; then the target that puts the look's
        // panel, not its shadow's margin, against it.
        const Rect& r = request.anchor;
        const juce::Rectangle<int> anchor = owner.localAreaToGlobal(
            juce::Rectangle<float>(r.x * scale, r.y * scale, r.w * scale, r.h * scale).toNearestInt());
        const float menuScale = juce::Component::getApproximateScaleFactorForComponent(&owner);
        // One column, which scrolls (MenuLook); a dropdown's width and current choice.
        juce::PopupMenu::Options options = juce::PopupMenu::Options()
                                               .withTargetComponent(&owner)
                                               .withTargetScreenArea(look.targetAreaFor(anchor, menuScale))
                                               .withMaximumNumColumns(1);
        if (request.minWidth > 0.0f)       // in the window's px, which are the owner's (the comment at the top)
            options = options.withMinimumWidth(juce::roundToInt(request.minWidth * scale) + 2 * look.shadowPx());
        if (request.selectedId > 0)
            options = options.withInitiallySelectedItem(request.selectedId).withItemThatMustBeVisible(request.selectedId);
        return options;
    }

    void HostServicesJuce::dismissMenus()
    {
        Pending& p = *pending_;
        if (!p.menuOpen)
            return;
        p.menuOpen = false;
        p.menu = nullptr;                            // dropped unrun
        juce::PopupMenu::dismissAllActiveMenus();
    }

    bool HostServicesJuce::chooseFiles(const FileRequest& request, FilesCallback done)
    {
        using FB = juce::FileBrowserComponent;
        const bool save = request.mode == FileRequest::Mode::save;
        const juce::String extension = save ? saveExtension(request.pattern) : juce::String();

        juce::File initial = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
        if (save && !request.suggestedName.empty())
        {
            const juce::String name = fromUtf8(request.suggestedName);
            const bool hasExtension = extension.isNotEmpty() && name.length() > extension.length()
                                      && name.endsWithIgnoreCase(extension);
            initial = initial.getChildFile(
                hasExtension ? juce::File::createLegalFileName(name.dropLastCharacters(extension.length()))
                                   + name.getLastCharacters(extension.length())
                             : juce::File::createLegalFileName(name));
        }
        int flags = FB::canSelectFiles;
        if (save)
            flags |= FB::saveMode | FB::warnAboutOverwriting;
        else if (request.mode == FileRequest::Mode::openMany)
            flags |= FB::openMode | FB::canSelectMultipleItems;
        else
            flags |= FB::openMode;

        Pending& p = *pending_;
        if (p.filesOpen)
            chooser_.reset();                        // the chooser still open is replaced: closed, its callback unrun
        else
            retired_ = std::move(chooser_);          // one that has finished may be the one calling back right now
        const uint32_t serial = ++p.filesSerial;
        p.files = std::move(done);
        p.filesOpen = true;
        const auto finish = [pending = pending_, serial](const std::vector<std::string>& paths) {
            if (!pending->filesOpen || pending->filesSerial != serial)
                return;                              // replaced or let go since
            pending->filesOpen = false;
            const FilesCallback callback = std::exchange(pending->files, nullptr);
            if (callback)
                callback(paths);
        };
        chooser_ = std::make_unique<juce::FileChooser>(fromUtf8(request.title), initial, fromUtf8(request.pattern),
                                                       true, false, &owner_);   // native, on the editor
        p.asking = true;
        chooser_->launchAsync(flags, [pending = pending_, finish, save, extension](const juce::FileChooser& fc) {
            std::vector<std::string> paths;
            if (save)
            {
                juce::File f = fc.getResult();
                if (f != juce::File())               // else cancelled
                {
                    if (extension.isNotEmpty() && !f.hasFileExtension(extension))
                        f = f.withFileExtension(extension);
                    paths.push_back(f.getFullPathName().toStdString());
                }
            }
            else
            {
                for (const juce::File& f : fc.getResults())
                    paths.push_back(f.getFullPathName().toStdString());
            }
            if (pending->asking)
                juce::MessageManager::callAsync([finish, paths] { finish(paths); });
            else
                finish(paths);
        });
        p.asking = false;
        return true;
    }

    bool HostServicesJuce::copyText(std::string_view utf8)
    {
        juce::SystemClipboard::copyTextToClipboard(fromUtf8(utf8));
        return true;
    }

    // FSynth G1: the read. One JUCE call, as the write above is; no fg test makes it (a test never touches the user's
    // clipboard, and JUCE's X11 calls need an open display).
    std::string HostServicesJuce::clipboardText() const
    {
        return juce::SystemClipboard::getTextFromClipboard().toStdString();
    }

    void HostServicesJuce::letGo()
    {
        dismissMenus();
        Pending& p = *pending_;
        p.filesOpen = false;
        p.files = nullptr;
        chooser_.reset();
        retired_.reset();
    }
}
