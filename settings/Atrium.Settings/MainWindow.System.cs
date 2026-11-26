using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace Atrium.Settings;

// Administrator privileges card in Settings -> System, plus elevated restart/exit helpers.
public sealed partial class MainWindow
{
    private TextBlock? _adminStatus;
    private Button? _adminButton;
    private bool _adminBuilt;

    private void EnsureAdminCard()
    {
        if (!_adminBuilt)
        {
            _adminBuilt = true;
            var header = new TextBlock
            {
                Text = Loc.T("Administrator privileges"),
                Style = (Style)Application.Current.Resources["SectionStyle"],
            };
            var title = new TextBlock
            {
                Text = Loc.T("Run as administrator"),
                Style = (Style)Application.Current.Resources["RowTitleStyle"],
            };
            var note = new TextBlock
            {
                Text = Loc.T("Manage elevated apps in Stage Manager, previews and tray. Prompts UAC; Explorer cannot drag onto an elevated taskbar."),
                Style = (Style)Application.Current.Resources["RowNoteStyle"],
                TextWrapping = TextWrapping.Wrap,
            };
            _adminStatus = new TextBlock
            {
                Style = (Style)Application.Current.Resources["RowNoteStyle"],
                Margin = new Thickness(0, 4, 0, 0),
            };
            _adminButton = new Button
            {
                VerticalAlignment = VerticalAlignment.Center,
                MinWidth = 140,
            };
            _adminButton.Click += OnRestartAsAdminClick;

            var left = new StackPanel { Spacing = 4, Children = { title, note, _adminStatus } };
            var grid = new Grid { ColumnSpacing = 16 };
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            Grid.SetColumn(left, 0);
            Grid.SetColumn(_adminButton, 1);
            grid.Children.Add(left);
            grid.Children.Add(_adminButton);

            var card = new Border
            {
                Style = (Style)Application.Current.Resources["TileStyle"],
                Child = grid,
            };
            MainSystem.Children.Add(new StackPanel { Children = { header, card } });
        }
        RefreshAdminCard();
    }

    private void DetachCoreProcess()
    {
        _coreProcess?.Dispose();
        _coreProcess = null;
    }

    private void RefreshAdminCard()
    {
        if (_adminStatus == null || _adminButton == null) return;
        bool running = _client.Connected;
        bool elevated = running && _client.CoreElevated;
        bool canRun = CoreClient.CoreExe() != null && !_coreChanging;
        _adminStatus.Text = elevated ? Loc.T("Running as administrator")
            : running ? Loc.T("Running as standard user")
            : Loc.T("Atrium is not running");
        _adminButton.Content = elevated ? Loc.T("Running as administrator")
            : running ? Loc.T("Restart as administrator")
            : Loc.T("Start as administrator");
        _adminButton.IsEnabled = canRun && !elevated;
    }

    private async Task<bool> WaitForCoreExitAsync(string exe, bool sentPipeQuit)
    {
        if (sentPipeQuit)
        {
            for (int i = 0; i < 40; i++)
            {
                if (_coreProcess == null || _coreProcess.HasExited) return true;
                await Task.Delay(150);
            }
        }
        using var quit = Process.Start(new ProcessStartInfo(exe, "--exit") { UseShellExecute = false, CreateNoWindow = true })
            ?? throw new InvalidOperationException(Loc.T("Could not send the quit request."));
        using var waitTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        try { await quit.WaitForExitAsync(waitTimeout.Token); }
        catch (OperationCanceledException) { throw new TimeoutException(Loc.T("Atrium has not quit yet. Try again later.")); }
        return quit.ExitCode == 0;
    }

    private async void OnRestartAsAdminClick(object sender, RoutedEventArgs e)
    {
        if (_coreChanging || (_client.Connected && _client.CoreElevated)) return;
        string? exe = CoreClient.CoreExe();
        if (exe == null) return;
        if (_session.HasUnsavedChanges && !await SaveAsync()) return;

        _coreChanging = true;
        RefreshAdminCard();
        CoreSwitch.IsEnabled = false;
        Status.IsOpen = false;
        string? previousId = _client.InstanceId;
        bool wasConnected = _client.Connected;
        try
        {
            using var launched = Process.Start(CoreClient.CoreStartInfo(exe, runAsAdmin: true, replace: true));
            if (wasConnected) _client.Disconnect();
            bool reconnected = false;
            for (int i = 0; i < 66 && !_updatesClosed; i++)
            {
                if (await _client.ConnectAsync(150))
                {
                    await SyncConnectedStateAsync();
                    if (_client.CoreElevated || (_client.InstanceId != null && _client.InstanceId != previousId))
                    {
                        reconnected = true;
                        break;
                    }
                    _client.Disconnect();
                }
                await Task.Delay(150);
            }
            if (!reconnected)
            {
                ShowMessage(InfoBarSeverity.Warning, Loc.T("Could not start as administrator"),
                    Loc.T("Atrium restarted, but the settings window could not reconnect yet. Check the status indicator."));
            }
            else if (_client.CoreElevated)
            {
                WatchCore();
                ShowMessage(InfoBarSeverity.Success, Loc.T("Administrator mode active"),
                    Loc.T("Atrium is now running with administrator privileges."));
            }
        }
        catch (System.ComponentModel.Win32Exception error) when (error.NativeErrorCode == 1223)
        {
            // UAC cancelled by the user: leave the running instance undisturbed.
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException or IOException)
        {
            ShowMessage(InfoBarSeverity.Error, Loc.T("Could not start as administrator"), error.Message);
        }
        finally
        {
            FinishCoreChange(watch: true);
            RefreshAdminCard();
        }
    }
}
