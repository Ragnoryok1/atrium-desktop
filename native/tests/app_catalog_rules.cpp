#include "shell/app_catalog_rules.h"

#include <cstdio>

namespace {

int g_failures = 0;

void Require(bool ok, const char* message)
{
    if (ok)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void TestWordBoundaryFilters()
{
    const atrium::CatalogApp goodHelper{L"Quick Helper", L"C:\\Programs\\QuickHelper.lnk", L"", L"C:\\Program Files\\QuickHelper\\helper.exe", L"", 0, false};
    Require(atrium::AppCatalog::IsLaunchable(goodHelper), "English app containing 'Helper' must stay launchable");

    const atrium::CatalogApp genuineHelp{L"Foo Help", L"C:\\Programs\\FooHelp.lnk", L"", L"C:\\Program Files\\Foo\\foo.exe", L"", 0, false};
    Require(!atrium::AppCatalog::IsLaunchable(genuineHelp), "genuine Help shortcut must still be excluded");

    const atrium::CatalogApp readMe{L"Read Me", L"C:\\Programs\\ReadMe.lnk", L"", L"C:\\Program Files\\Foo\\foo.exe", L"", 0, false};
    Require(!atrium::AppCatalog::IsLaunchable(readMe), "'Read Me' shortcut must still be excluded");

    Require(!atrium::CatalogByMaker(atrium::FoldCatalogText(L"C:\\Program Files\\JetBrains\\IntelliJ IDEA")),
            "maker word boundary must not match 'intel' inside 'IntelliJ'");
    Require(!atrium::CatalogByMaker(atrium::FoldCatalogText(L"3D Modeller Studio")), "maker word boundary must not match 'dell' inside 'Modeller'");
    Require(atrium::CatalogByMaker(atrium::FoldCatalogText(L"Intel Graphics Command Center")), "maker word boundary must match standalone 'Intel'");
    Require(atrium::CatalogByMaker(atrium::FoldCatalogText(L"Dell SupportAssist")), "maker word boundary must match standalone 'Dell'");
    Require(atrium::CatalogByMaker(atrium::FoldCatalogText(L"ASUSTeK Computer Inc.")), "maker word boundary must match ASUSTeK");
    Require(atrium::CatalogByMaker(atrium::FoldCatalogText(L"Advanced Micro Devices, Inc.")), "maker word boundary must match AMD");
    Require(!atrium::CatalogByMaker(atrium::FoldCatalogText(L"PHPStorm")), "maker 'hp' must not match inside 'PHPStorm'");
    Require(!atrium::CatalogByMaker(atrium::FoldCatalogText(L"MSIX Packaging Tool")), "maker 'msi' must not match inside 'MSIX'");
    Require(!atrium::CatalogByMaker(atrium::FoldCatalogText(L"Lambda Studio")), "maker 'amd' must not match inside 'Lambda'");

    const atrium::CatalogApp batApp{L"WNG Script", L"C:\\Start\\WNG.lnk", L"", L"C:\\Program Files\\ZTE\\WNG\\start.bat", L"", 0, false};
    const atrium::CatalogApp jarApp{L"Java Tool", L"C:\\Start\\JavaTool.lnk", L"", L"C:\\Tools\\tool.jar", L"", 0, false};
    const atrium::CatalogApp clickOnce{L"ClickOnce App", L"C:\\Start\\App.appref-ms", L"", L"C:\\Start\\App.appref-ms", L"", 0, false};
    Require(atrium::AppCatalog::IsLaunchable(batApp) && atrium::AppCatalog::IsLaunchable(jarApp) && atrium::AppCatalog::IsLaunchable(clickOnce),
            "script, jar, and ClickOnce app shortcuts stay launchable");

    Require(atrium::IsStartupStartFolder(L"startup") && atrium::IsStartupStartFolder(L"启动"), "Startup folder detected separately");
    Require(!atrium::IsSystemStartFolder(L"startup"), "Startup folder must not mark user apps as system apps");
    Require(atrium::IsSystemStartFolder(L"windows accessories") && atrium::IsSystemStartFolder(L"windows 管理工具") &&
                atrium::IsToolStartFolder(L"microsoft office 工具"),
            "Windows and Office localized utility folders stay in system folder sets");

    Require(atrium::IsVisualStudioInstallerApp(L"microsoft.visualstudio.installer", L"") &&
                atrium::IsVisualStudioInstallerApp(L"custom.id", L"c:\\program files (x86)\\microsoft visual studio\\installer\\setup.exe") &&
                !atrium::IsVisualStudioInstallerApp(L"devenv", L"c:\\program files\\microsoft visual studio\\2022\\common7\\ide\\devenv.exe"),
            "Visual Studio Installer routes to system apps without catching Visual Studio IDE");

    Require(atrium::IsBuiltInMicrosoftApp(L"microsoft.windowsnotepad_8wekyb3d8bbwe!app", L"") &&
                atrium::IsBuiltInMicrosoftApp(L"windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel", L"") &&
                atrium::IsBuiltInMicrosoftApp(L"msedge", L"") && atrium::IsBuiltInMicrosoftApp(L"custom", L"microsoft.skydrive.desktop") &&
                !atrium::IsBuiltInMicrosoftApp(L"microsoft.office.winword.exe.15", L"") && !atrium::IsBuiltInMicrosoftApp(L"chrome", L""),
            "built-in Microsoft utility/inbox apps group separately from third-party and Office desktop apps");
}

void TestStartMenuShortcutMergeAndDedup()
{
    std::vector<atrium::CatalogApp> apps{
        {L"Browser App A", L"MSEdge", L"MSEdge", L"C:\\Program Files\\Edge\\msedge.exe", L"--app-id=aaa", 0, false},
        {L"Existing Tool", L"C:\\Existing\\Tool.lnk", L"", L"C:\\Program Files\\Tool\\tool.exe", L"", 0, false},
        {L"Chat WebApp", L"C:\\Start\\ChatWeb.lnk", L"", L"C:\\Program Files\\Chrome\\chrome.exe", L"--app-id=chat --source-shortcut-location=start-menu", 0,
         false},
    };
    const std::vector<atrium::CatalogApp> shortcuts{
        // Missing electron app from Start Menu Programs -> should be merged.
        {L"lx-music-desktop", L"C:\\Start\\lx-music-desktop.lnk", L"org.lxmusic.desktop", L"C:\\Users\\u\\AppData\\Local\\Programs\\lx-music\\lx-music.exe",
         L"", 100, false},
        // Same target + same args as Existing Tool -> duplicate, skip.
        {L"Existing Tool Duplicate", L"C:\\Start\\Tool.lnk", L"", L"c:/program files/tool/tool.exe", L"  ", 100, false},
        // Desktop copy with --source-shortcut-location=desktop -> duplicate, skip.
        {L"Chat WebApp", L"C:\\Desktop\\ChatWeb.lnk", L"", L"C:\\Program Files\\Chrome\\chrome.exe", L"--app-id=chat --source-shortcut-location=desktop", 200,
         false},
        // Same target exe, different --app-id argument -> keep distinct entry.
        {L"Browser App B", L"C:\\Start\\AppB.lnk", L"", L"C:\\Program Files\\Edge\\msedge.exe", L"--app-id=bbb", 100, false},
        // Same AUMID -> duplicate, skip.
        {L"Edge Alias", L"C:\\Start\\EdgeAlias.lnk", L"msedge", L"C:\\Other\\edge.exe", L"", 100, false},
        // Non-launchable help shortcut -> skip.
        {L"Product Help", L"C:\\Start\\Help.lnk", L"", L"C:\\Program Files\\Tool\\help.exe", L"", 100, false},
    };

    const bool added = atrium::MergeStartMenuShortcuts(apps, shortcuts, &atrium::AppCatalog::IsLaunchable);
    Require(added, "MergeStartMenuShortcuts must report added shortcuts");
    Require(apps.size() == 5, "expected 2 merged entries (lx-music-desktop + Browser App B), total 5");
    Require(apps[3].name == L"lx-music-desktop" && apps[4].name == L"Browser App B", "merged entries must preserve display names and distinct args");
}

} // namespace

int main()
{
    TestWordBoundaryFilters();
    TestStartMenuShortcutMergeAndDedup();
    if (g_failures > 0)
        return 1;
    std::printf("app catalog rules: all checks passed\n");
    return 0;
}
