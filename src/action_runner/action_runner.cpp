#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>

#include <string_view>

#include <common/common.h>
#include <common/updating/updating.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <Msi.h>

#include "../runner/tray_icon.h"
#include "../runner/action_runner_utils.h"

#include "resource.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

int uninstall_msi_action()
{
    const auto package_path = updating::get_msi_package_path();
    if (package_path.empty())
    {
        return 0;
    }
    if (!updating::uninstall_msi_version(package_path))
    {
        return -1;
    }

    // Launch PowerToys again, since it's been terminated by the MSI uninstaller
    std::wstring runner_path{ winrt::Windows::ApplicationModel::Package::Current().InstalledLocation().Path() };
    runner_path += L"\\PowerToys.exe";
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = { SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC };
    sei.lpFile = runner_path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);

    return 0;
}

namespace fs = std::filesystem;

std::optional<fs::path> copy_self_to_temp_dir()
{
    std::error_code error;
    auto dst_path = fs::temp_directory_path() / "action_runner.exe";
    fs::copy_file(get_module_filename(), dst_path, fs::copy_options::overwrite_existing, error);
    if (error)
    {
        return std::nullopt;
    }
    return std::move(dst_path);
}

bool install_new_version_stage_1(const bool must_restart = false)
{
    std::optional<fs::path> installer;
    for (auto path : fs::directory_iterator{ updating::get_pending_updates_path() })
    {
        if (path.path().native().find(updating::installer_filename_pattern) != std::wstring::npos)
        {
            installer.emplace(std::move(path));
            break;
        }
    }
    if (!installer)
    {
        return false;
    }

    if (auto copy_in_temp = copy_self_to_temp_dir())
    {
        // detect if PT was running
        const auto pt_main_window = FindWindowW(pt_tray_icon_window_class, nullptr);
        const bool launch_powertoys = must_restart || pt_main_window != nullptr;
        if (pt_main_window != nullptr)
        {
            SendMessageW(pt_main_window, WM_CLOSE, 0, 0);
        }

        std::wstring arguments{ UPDATE_NOW_LAUNCH_STAGE2_CMDARG };
        arguments += L" \"";
        arguments += installer->c_str();
        arguments += L"\" \"";
        arguments += get_module_folderpath();
        arguments += L"\" ";
        arguments += launch_powertoys ? UPDATE_STAGE2_RESTART_PT_CMDARG : UPDATE_STAGE2_DONT_START_PT_CMDARG;
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.fMask = { SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC };
        sei.lpFile = copy_in_temp->c_str();
        sei.nShow = SW_SHOWNORMAL;

        sei.lpParameters = arguments.c_str();
        return ShellExecuteExW(&sei) == TRUE;
    }
    else
    {
        return false;
    }
}

bool install_new_version_stage_2(std::wstring_view installer_path, std::wstring_view install_path, const bool launch_powertoys)
{
    if (MsiInstallProductW(installer_path.data(), nullptr) != ERROR_SUCCESS)
    {
        return false;
    }

    std::error_code _;
    fs::remove(installer_path, _);
    if (launch_powertoys)
    {
        std::wstring new_pt_path{ install_path };
        new_pt_path += L"\\PowerToys.exe";
        SHELLEXECUTEINFOW sei{ sizeof(sei) };
        sei.fMask = { SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC };
        sei.lpFile = new_pt_path.c_str();
        sei.nShow = SW_SHOWNORMAL;
        sei.lpParameters = UPDATE_REPORT_SUCCESS;
        return ShellExecuteExW(&sei) == TRUE;
    }
    return true;
}

bool dotnet_is_installed()
{
    auto runtimes = exec_and_read_output(LR"(dotnet --list-runtimes)");
    if (!runtimes)
    {
        return false;
    }
    const char DESKTOP_DOTNET_RUNTIME_STRING[] = "Microsoft.WindowsDesktop.App 3.0.";
    return runtimes->find(DESKTOP_DOTNET_RUNTIME_STRING) != std::string::npos;
}

bool install_dotnet(std::wstring_view installer_download_link, std::wstring_view installer_filename)
{
    auto dotnet_download_path = fs::temp_directory_path() / installer_filename;
    winrt::Windows::Foundation::Uri download_link{ installer_download_link };

    const size_t max_attempts = 3;
    bool download_success = false;
    for (size_t i = 0; i < max_attempts; ++i)
    {
        try
        {
            updating::try_download_file(dotnet_download_path, download_link).wait();
            download_success = true;
            break;
        }
        catch (...)
        {
            // couldn't download
        }
    }
    if (!download_success)
    {
        MessageBoxW(nullptr,
                    GET_RESOURCE_STRING(IDS_DOTNET_CORE_DOWNLOAD_FAILURE).c_str(),
                    GET_RESOURCE_STRING(IDS_DOTNET_CORE_DOWNLOAD_FAILURE_TITLE).c_str(),
                    MB_OK | MB_ICONERROR);
        return false;
    }
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = { SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS };
    sei.lpFile = dotnet_download_path.c_str();
    sei.nShow = SW_SHOWNORMAL;
    sei.lpParameters = L"/install /passive";
    if (ShellExecuteExW(&sei) != TRUE)
    {
        return false;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    CloseHandle(sei.hProcess);
    return true;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    int nArgs = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &nArgs);
    if (!args || nArgs < 2)
    {
        return 1;
    }
    std::wstring_view action{ args[1] };

    if (action == L"-install_dotnet")
    {
        if (dotnet_is_installed())
        {
            return 0;
        }

        using installer_link_and_filename_t = std::pair<std::wstring_view, std::wstring_view>;

        const wchar_t DOTNET_CORE_DOWNLOAD_LINK[] = L"https://download.visualstudio.microsoft.com/download/pr/fa69f1ae-255d-453c-b4ff-28d832525037/51694be04e411600c2e3361f6c81400d/dotnet-runtime-3.0.3-win-x64.exe";
        const wchar_t DOTNET_CORE_INSTALLER_NAME[] = L"dotnet-runtime-3.0.3-win-x64.exe";

        const wchar_t DOTNET_DESKTOP_DOWNLOAD_LINK[] = L"https://download.visualstudio.microsoft.com/download/pr/c525a2bb-6e98-4e6e-849e-45241d0db71c/d21612f02b9cae52fa50eb54de905986/windowsdesktop-runtime-3.0.3-win-x64.exe";
        const wchar_t DOTNET_DESKTOP_INSTALLER_NAME[] = L"windowsdesktop-runtime-3.0.3-win-x64.exe";

        const std::array<installer_link_and_filename_t, 2> dotnet_installers = {
            installer_link_and_filename_t{ DOTNET_CORE_DOWNLOAD_LINK,
                                           DOTNET_CORE_INSTALLER_NAME },
            installer_link_and_filename_t{ DOTNET_DESKTOP_DOWNLOAD_LINK,
                                           DOTNET_DESKTOP_INSTALLER_NAME }
        };

        for (const auto [installer_link, installer_filename] : dotnet_installers)
        {
            if (!install_dotnet(installer_link, installer_filename))
            {
                return 1;
            }
        }
        return 0;
    }
    else if (action == L"-uninstall_msi")
    {
        return uninstall_msi_action();
    }
    else if (action == UPDATE_NOW_LAUNCH_STAGE1_CMDARG)
    {
        return !install_new_version_stage_1();
    }
    else if (action == UPDATE_NOW_LAUNCH_STAGE1_START_PT_CMDARG)
    {
        return !install_new_version_stage_1(true);
    }
    else if (action == UPDATE_NOW_LAUNCH_STAGE2_CMDARG)
    {
        using namespace std::string_view_literals;
        return !install_new_version_stage_2(args[2], args[3], args[4] == std::wstring_view{ UPDATE_STAGE2_RESTART_PT_CMDARG });
    }

    return 0;
}