// SG Store: unattended Qt Installer Framework setup (installer and maintenance tool), passed as
// "--script Z:\usr\share\stained-glass\store\qtifw-auto.qs" by catalogue entries whose InstallArgs and
// UninstallArgs name it. IFW 3 installers ignore /S and have no other unattended switch.
function Controller()
{
    installer.autoRejectMessageBoxes();
    installer.setMessageBoxAutomaticAnswer("OverwriteTargetDirectory", QMessageBox.Yes);
    installer.setMessageBoxAutomaticAnswer("stopProcessesForUpdates", QMessageBox.Ignore);
    installer.installationFinished.connect(function() { gui.clickButton(buttons.NextButton); });
    installer.uninstallationFinished.connect(function() { gui.clickButton(buttons.NextButton); });
}
function next() { gui.clickButton(buttons.NextButton); }
Controller.prototype.WelcomePageCallback = function() { gui.clickButton(buttons.NextButton, 3000); };
Controller.prototype.CredentialsPageCallback = next;
Controller.prototype.IntroductionPageCallback = function()
{
    var page = gui.currentPageWidget();
    if (installer.isUninstaller() && page && page.UninstallerRadioButton)
        page.UninstallerRadioButton.setChecked(true);
    next();
};
Controller.prototype.TargetDirectoryPageCallback = next;
Controller.prototype.ComponentSelectionPageCallback = next;
Controller.prototype.LicenseAgreementPageCallback = function()
{
    var page = gui.currentPageWidget();
    if (page && page.AcceptLicenseRadioButton) page.AcceptLicenseRadioButton.setChecked(true);
    if (page && page.AcceptLicenseCheckBox) page.AcceptLicenseCheckBox.setChecked(true);
    next();
};
Controller.prototype.StartMenuDirectoryPageCallback = next;
Controller.prototype.ReadyForInstallationPageCallback = next;
Controller.prototype.PerformInstallationPageCallback = function() {};
Controller.prototype.FinishedPageCallback = function()
{
    var page = gui.currentPageWidget();
    if (page && page.RunItCheckBox) page.RunItCheckBox.setChecked(false);
    gui.clickButton(buttons.FinishButton);
};
