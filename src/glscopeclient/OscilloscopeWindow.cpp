/***********************************************************************************************************************
*                                                                                                                      *
* glscopeclient                                                                                                        *
*                                                                                                                      *
* Copyright (c) 2012-2022 Andrew D. Zonenberg                                                                          *
* All rights reserved.                                                                                                 *
*                                                                                                                      *
* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the     *
* following conditions are met:                                                                                        *
*                                                                                                                      *
*    * Redistributions of source code must retain the above copyright notice, this list of conditions, and the         *
*      following disclaimer.                                                                                           *
*                                                                                                                      *
*    * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the       *
*      following disclaimer in the documentation and/or other materials provided with the distribution.                *
*                                                                                                                      *
*    * Neither the name of the author nor the names of any contributors may be used to endorse or promote products     *
*      derived from this software without specific prior written permission.                                           *
*                                                                                                                      *
* THIS SOFTWARE IS PROVIDED BY THE AUTHORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED   *
* TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL *
* THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES        *
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR       *
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT *
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE       *
* POSSIBILITY OF SUCH DAMAGE.                                                                                          *
*                                                                                                                      *
***********************************************************************************************************************/

/**
	@file
	@author Andrew D. Zonenberg
	@brief Implementation of main application window class
 */

#include "glscopeclient.h"
#include "glscopeclient-version.h"
#include "../scopehal/Instrument.h"
#include "../scopehal/MockOscilloscope.h"
#include "OscilloscopeWindow.h"
#include "PreferenceDialog.h"
#include "InstrumentConnectionDialog.h"
#include "TriggerPropertiesDialog.h"
#include "TimebasePropertiesDialog.h"
#include "FileProgressDialog.h"
#include "MultimeterDialog.h"
#include "FunctionGeneratorDialog.h"
#include "FileSystem.h"
#include <unistd.h>
#include <fcntl.h>
#include "../../lib/scopeprotocols/EyePattern.h"
#include "../../lib/scopeprotocols/SpectrogramFilter.h"

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#else
#include <sys/mman.h>
#endif

using namespace std;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Construction / destruction

/**
	@brief Initializes the main window
 */
OscilloscopeWindow::OscilloscopeWindow(const vector<Oscilloscope*>& scopes, bool nodigital, bool nospectrum)
	: m_exportWizard(nullptr)
	, m_scopes(scopes)
	, m_fullscreen(false)
	, m_multiScopeFreeRun(false)
	, m_scopeSyncWizard(NULL)
	, m_syncComplete(false)
	, m_graphEditor(NULL)
	, m_haltConditionsDialog(this)
	, m_timebasePropertiesDialog(NULL)
	, m_addFilterDialog(NULL)
	, m_pendingGenerator(NULL)
	, m_triggerArmed(false)
	, m_triggerOneShot(false)
	, m_shuttingDown(false)
	, m_loadInProgress(false)
	, m_waveformProcessingThread(WaveformProcessingThread, this)
{
	SetTitle();
	FindScopeFuncGens();

	//Initial setup
	set_reallocate_redraws(true);
	set_default_size(1280, 800);

	//Add widgets
	CreateWidgets(nodigital, nospectrum);

	//Update recently used instrument list
	LoadRecentlyUsedList();
	AddCurrentToRecentlyUsedList();
	SaveRecentlyUsedList();
	RefreshInstrumentMenu();

	ArmTrigger(TRIGGER_TYPE_NORMAL);
	m_toggleInProgress = false;

	m_tLastFlush = GetTime();

	m_totalWaveforms = 0;

	//Start a timer for polling for scope updates
	//TODO: can we use signals of some sort to avoid busy polling until a trigger event?
	Glib::signal_timeout().connect(sigc::bind(sigc::mem_fun(*this, &OscilloscopeWindow::OnTimer), 1), 5);
}

void OscilloscopeWindow::SetTitle()
{
	if(m_scopes.empty())
	{
		set_title("glscopeclient [OFFLINE]");
		return;
	}

	//Set title
	string title = "glscopeclient: ";
	for(size_t i=0; i<m_scopes.size(); i++)
	{
		auto scope = m_scopes[i];

		//Redact serial number upon request
		string serial = scope->GetSerial();
		if(GetPreferences().GetBool("Privacy.redact_serial_in_title"))
		{
			for(int j=serial.length()-3; j >= 0; j--)
				serial[j] = '*';
		}

		char tt[256];
		snprintf(tt, sizeof(tt), "%s (%s %s, serial %s)",
			scope->m_nickname.c_str(),
			scope->GetVendor().c_str(),
			scope->GetName().c_str(),
			serial.c_str()
			);

		if(i > 0)
			title += ", ";
		title += tt;

		if(dynamic_cast<MockOscilloscope*>(scope) != NULL)
			title += "[OFFLINE]";
	}

	#ifdef _DEBUG
		title += " [DEBUG BUILD]";
	#endif

	set_title(title);
}

/**
	@brief Application cleanup
 */
OscilloscopeWindow::~OscilloscopeWindow()
{
	//Terminate the waveform processing thread
	g_waveformProcessedEvent.Signal();
	m_waveformProcessingThread.join();
}

/**
	@brief Helper function for creating widgets and setting up signal handlers
 */
void OscilloscopeWindow::CreateWidgets(bool nodigital, bool nospectrum)
{
	//Initialize filter colors from preferences
	SyncFilterColors();

	//Initialize color ramps
	m_eyeColor = "KRain";
	m_eyeFiles["CRT"] = FindDataFile("gradients/eye-gradient-crt.rgba");
	m_eyeFiles["Ironbow"] = FindDataFile("gradients/eye-gradient-ironbow.rgba");
	m_eyeFiles["Rainbow"] = FindDataFile("gradients/eye-gradient-rainbow.rgba");
	m_eyeFiles["Reverse Rainbow"] = FindDataFile("gradients/eye-gradient-reverse-rainbow.rgba");
	m_eyeFiles["Viridis"] = FindDataFile("gradients/eye-gradient-viridis.rgba");
	m_eyeFiles["Grayscale"] = FindDataFile("gradients/eye-gradient-grayscale.rgba");
	m_eyeFiles["KRain"] = FindDataFile("gradients/eye-gradient-krain.rgba");

	//Set up window hierarchy
	add(m_vbox);
		m_vbox.pack_start(m_menu, Gtk::PACK_SHRINK);
			m_menu.append(m_fileMenuItem);
				m_fileMenuItem.set_label("File");
				m_fileMenuItem.set_submenu(m_fileMenu);

					Gtk::MenuItem* item = Gtk::manage(new Gtk::MenuItem("Connect...", false));
					item->signal_activate().connect(
						sigc::mem_fun(*this, &OscilloscopeWindow::OnFileConnect));
					m_fileMenu.append(*item);
					m_recentInstrumentsMenuItem.set_label("Recent Instruments");
					m_recentInstrumentsMenuItem.set_submenu(m_recentInstrumentsMenu);
					m_fileMenu.append(m_recentInstrumentsMenuItem);

					item = Gtk::manage(new Gtk::SeparatorMenuItem);
					m_fileMenu.append(*item);

					item = Gtk::manage(new Gtk::MenuItem("Open...", false));
					item->signal_activate().connect(
						sigc::mem_fun(*this, &OscilloscopeWindow::OnFileOpen));
					m_fileMenu.append(*item);
					item = Gtk::manage(new Gtk::MenuItem("Import...", false));
					item->signal_activate().connect(
						sigc::mem_fun(*this, &OscilloscopeWindow::OnFileImport));
					m_fileMenu.append(*item);

					item = Gtk::manage(new Gtk::SeparatorMenuItem);
					m_fileMenu.append(*item);

					item = Gtk::manage(new Gtk::MenuItem("Save Layout Only", false));
					item->signal_activate().connect(
						sigc::bind<bool, bool, bool>(
							sigc::mem_fun(*this, &OscilloscopeWindow::OnFileSave),
							true, true, false));
					m_fileMenu.append(*item);
					item = Gtk::manage(new Gtk::MenuItem("Save Layout Only As...", false));
					item->signal_activate().connect(
						sigc::bind<bool, bool, bool>(
							sigc::mem_fun(*this, &OscilloscopeWindow::OnFileSave),
							false, true, false));
					m_fileMenu.append(*item);
					item = Gtk::manage(new Gtk::MenuItem("Save Layout and Waveforms", false));
					item->signal_activate().connect(
						sigc::bind<bool, bool, bool>(
							sigc::mem_fun(*this, &OscilloscopeWindow::OnFileSave),
							true, true, true));
					m_fileMenu.append(*item);
					item = Gtk::manage(new Gtk::MenuItem("Save Layout and Waveforms As...", false));
					item->signal_activate().connect(
						sigc::bind<bool, bool, bool>(
							sigc::mem_fun(*this, &OscilloscopeWindow::OnFileSave),
							false, true, true));
					m_fileMenu.append(*item);

					item = Gtk::manage(new Gtk::SeparatorMenuItem);
					m_fileMenu.append(*item);

					m_exportMenuItem.set_label("Export");
					m_exportMenuItem.set_submenu(m_exportMenu);
					m_fileMenu.append(m_exportMenuItem);

					item = Gtk::manage(new Gtk::SeparatorMenuItem);
					m_fileMenu.append(*item);

					item = Gtk::manage(new Gtk::MenuItem("Close", false));
					item->signal_activate().connect(
						sigc::mem_fun(*this, &OscilloscopeWindow::CloseSession));
					m_fileMenu.append(*item);

					item = Gtk::manage(new Gtk::SeparatorMenuItem);
					m_fileMenu.append(*item);

					item = Gtk::manage(new Gtk::MenuItem("Quit", false));
					item->signal_activate().connect(
						sigc::mem_fun(*this, &OscilloscopeWindow::OnQuit));
					m_fileMenu.append(*item);
			m_menu.append(m_setupMenuItem);
				m_setupMenuItem.set_label("Setup");
				m_setupMenuItem.set_submenu(m_setupMenu);
				m_setupMenu.append(m_setupSyncMenuItem);
					m_setupSyncMenuItem.set_label("Instrument Sync...");
					m_setupSyncMenuItem.signal_activate().connect(
						sigc::mem_fun(*this, &OscilloscopeWindow::OnScopeSync));
				m_setupMenu.append(m_setupTriggerMenuItem);
					m_setupTriggerMenuItem.set_label("Trigger");
					m_setupTriggerMenuItem.set_submenu(m_setupTriggerMenu);
				m_setupMenu.append(m_setupHaltMenuItem);
					m_setupHaltMenuItem.set_label("Halt Conditions...");
					m_setupHaltMenuItem.signal_activate().connect(
						sigc::mem_fun(*this, &OscilloscopeWindow::OnHaltConditions));
				m_setupMenu.append(m_preferencesMenuItem);
					m_preferencesMenuItem.set_label("Preferences");
					m_preferencesMenuItem.signal_activate().connect(
						sigc::mem_fun(*this, &OscilloscopeWindow::OnPreferences));
			m_menu.append(m_viewMenuItem);
				m_viewMenuItem.set_label("View");
				m_viewMenuItem.set_submenu(m_viewMenu);
					m_viewMenu.append(m_viewEyeColorMenuItem);
					m_viewEyeColorMenuItem.set_label("Color ramp");
					m_viewEyeColorMenuItem.set_submenu(m_viewEyeColorMenu);
						auto names = GetEyeColorNames();
						for(auto n : names)
						{
							auto eitem = Gtk::manage(new Gtk::RadioMenuItem);
							m_viewEyeColorMenu.append(*eitem);
							eitem->set_label(n);
							eitem->set_group(m_eyeColorGroup);
							eitem->signal_activate().connect(sigc::bind<std::string, Gtk::RadioMenuItem*>(
								sigc::mem_fun(*this, &OscilloscopeWindow::OnEyeColorChanged), n, eitem));
						}
						m_viewEyeColorMenu.show_all();
			m_menu.append(m_addMenuItem);
				m_addMenuItem.set_label("Add");
				m_addMenuItem.set_submenu(m_addMenu);
					m_addMenu.append(m_channelsMenuItem);
						m_channelsMenuItem.set_label("Channels");
						m_channelsMenuItem.set_submenu(m_channelsMenu);
					m_addMenu.append(m_generateMenuItem);
						m_generateMenuItem.set_label("Generate");
						m_generateMenuItem.set_submenu(m_generateMenu);
					m_addMenu.append(m_importMenuItem);
						m_importMenuItem.set_label("Import");
						m_importMenuItem.set_submenu(m_importMenu);
					RefreshGenerateAndImportMenu();
			m_menu.append(m_windowMenuItem);
				m_windowMenuItem.set_label("Window");
				m_windowMenuItem.set_submenu(m_windowMenu);
					m_windowMenu.append(m_windowFilterGraphItem);
						m_windowFilterGraphItem.set_label("Filter Graph");
						m_windowFilterGraphItem.signal_activate().connect(
							sigc::mem_fun(*this, &OscilloscopeWindow::OnFilterGraph));
					m_windowMenu.append(m_windowAnalyzerMenuItem);
						m_windowAnalyzerMenuItem.set_label("Analyzer");
						m_windowAnalyzerMenuItem.set_submenu(m_windowAnalyzerMenu);
					m_windowMenu.append(m_windowGeneratorMenuItem);
						m_windowGeneratorMenuItem.set_label("Generator");
						m_windowGeneratorMenuItem.set_submenu(m_windowGeneratorMenu);
					m_windowMenu.append(m_windowMultimeterMenuItem);
						m_windowMultimeterMenuItem.set_label("Multimeter");
						m_windowMultimeterMenuItem.set_submenu(m_windowMultimeterMenu);
			m_menu.append(m_helpMenuItem);
				m_helpMenuItem.set_label("Help");
				m_helpMenuItem.set_submenu(m_helpMenu);
					m_helpMenu.append(m_aboutMenuItem);
					m_aboutMenuItem.set_label("About...");
					m_aboutMenuItem.signal_activate().connect(
						sigc::mem_fun(*this, &OscilloscopeWindow::OnAboutDialog));

		m_vbox.pack_start(m_toolbox, Gtk::PACK_SHRINK);
			m_vbox.get_style_context()->add_class("toolbar");
			m_toolbox.pack_start(m_toolbar, Gtk::PACK_EXPAND_WIDGET);
				PopulateToolbar();
			m_toolbox.pack_start(m_alphalabel, Gtk::PACK_SHRINK);
				m_alphalabel.set_label("Opacity ");
				m_alphalabel.get_style_context()->add_class("toolbar");
			m_toolbox.pack_start(m_alphaslider, Gtk::PACK_SHRINK);
				m_alphaslider.set_size_request(200, 10);
				m_alphaslider.set_round_digits(3);
				m_alphaslider.set_draw_value(false);
				m_alphaslider.set_range(0, 0.75);
				m_alphaslider.set_increments(0.01, 0.01);
				m_alphaslider.set_margin_left(10);
				m_alphaslider.set_value(0.5);
				m_alphaslider.signal_value_changed().connect(
					sigc::mem_fun(*this, &OscilloscopeWindow::OnAlphaChanged));
				m_alphaslider.get_style_context()->add_class("toolbar");

		auto split = new Gtk::VPaned;
			m_vbox.pack_start(*split);
			m_splitters.emplace(split);

		m_vbox.pack_start(m_statusbar, Gtk::PACK_SHRINK);
			m_statusbar.get_style_context()->add_class("status");
			m_statusbar.pack_end(m_triggerConfigLabel, Gtk::PACK_SHRINK);
			m_triggerConfigLabel.set_size_request(75, 1);
			m_statusbar.pack_end(m_waveformRateLabel, Gtk::PACK_SHRINK);
			m_waveformRateLabel.set_size_request(175, 1);

	//Reconfigure menus
	RefreshChannelsMenu();
	RefreshMultimeterMenu();
	RefreshTriggerMenu();
	RefreshExportMenu();
	RefreshGeneratorsMenu();

	//History isn't shown by default
	for(auto it : m_historyWindows)
		it.second->hide();

	//Create the waveform areas for all enabled channels
	CreateDefaultWaveformAreas(split, nodigital, nospectrum);

	//Don't show measurements or wizards by default
	m_haltConditionsDialog.hide();

	//Initialize the style sheets
	m_css = Gtk::CssProvider::create();
	m_css->load_from_path(FindDataFile("styles/glscopeclient.css"));
	get_style_context()->add_provider_for_screen(
		Gdk::Screen::get_default(), m_css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/**
	@brief Populates the toolbar
 */
void OscilloscopeWindow::PopulateToolbar()
{
	//Remove all existing toolbar items
	auto children = m_toolbar.get_children();
	for(auto c : children)
		m_toolbar.remove(*c);

	int size = m_preferences.GetEnum<int>("Appearance.Toolbar.icon_size");

	//FindDataFile() assumes a file name, not a directory. Need to search for a specific file.
	//Then assume all other data files are in the same directory.
	//TODO: might be better to FindDataFile each one separately so we can override?
	string testfname = "fullscreen-enter.png";
	string base_path = FindDataFile("icons/" + to_string(size) + "x" + to_string(size) + "/" + testfname);
	base_path = base_path.substr(0, base_path.length() - testfname.length());

	m_iconEnterFullscreen = Gtk::Image(base_path + "fullscreen-enter.png");
	m_iconExitFullscreen = Gtk::Image(base_path + "fullscreen-exit.png");

	m_toolbar.set_toolbar_style(m_preferences.GetEnum<Gtk::ToolbarStyle>("Appearance.Toolbar.button_style"));

	m_toolbar.append(m_btnStart, sigc::mem_fun(*this, &OscilloscopeWindow::OnStart));
		m_btnStart.set_tooltip_text("Start (normal trigger)");
		m_btnStart.set_label("Start");
		m_btnStart.set_icon_widget(*Gtk::manage(new Gtk::Image(base_path + "trigger-start.png")));
	m_toolbar.append(m_btnStartSingle, sigc::mem_fun(*this, &OscilloscopeWindow::OnStartSingle));
		m_btnStartSingle.set_tooltip_text("Start (single trigger)");
		m_btnStartSingle.set_label("Single");
		m_btnStartSingle.set_icon_widget(*Gtk::manage(new Gtk::Image(base_path + "trigger-single.png")));
	m_toolbar.append(m_btnStartForce, sigc::mem_fun(*this, &OscilloscopeWindow::OnForceTrigger));
		m_btnStartForce.set_tooltip_text("Force trigger");
		m_btnStartForce.set_label("Force");
		m_btnStartForce.set_icon_widget(*Gtk::manage(new Gtk::Image(base_path + "trigger-single.png")));	//TODO
																											//draw icon
	m_toolbar.append(m_btnStop, sigc::mem_fun(*this, &OscilloscopeWindow::OnStop));
		m_btnStop.set_tooltip_text("Stop trigger");
		m_btnStop.set_label("Stop");
		m_btnStop.set_icon_widget(*Gtk::manage(new Gtk::Image(base_path + "trigger-stop.png")));
	m_toolbar.append(*Gtk::manage(new Gtk::SeparatorToolItem));
	m_toolbar.append(m_btnHistory, sigc::mem_fun(*this, &OscilloscopeWindow::OnHistory));
		m_btnHistory.set_tooltip_text("History");
		m_btnHistory.set_label("History");
		m_btnHistory.set_icon_widget(*Gtk::manage(new Gtk::Image(base_path + "history.png")));
	m_toolbar.append(*Gtk::manage(new Gtk::SeparatorToolItem));
	m_toolbar.append(m_btnRefresh, sigc::mem_fun(*this, &OscilloscopeWindow::OnRefreshConfig));
		m_btnRefresh.set_tooltip_text("Reload configuration from scope");
		m_btnRefresh.set_label("Reload Config");
		m_btnRefresh.set_icon_widget(*Gtk::manage(new Gtk::Image(base_path + "refresh-settings.png")));
	m_toolbar.append(m_btnClearSweeps, sigc::mem_fun(*this, &OscilloscopeWindow::OnClearSweeps));
		m_btnClearSweeps.set_tooltip_text("Clear sweeps");
		m_btnClearSweeps.set_label("Clear Sweeps");
		m_btnClearSweeps.set_icon_widget(*Gtk::manage(new Gtk::Image(base_path + "clear-sweeps.png")));
	m_toolbar.append(m_btnFullscreen, sigc::mem_fun(*this, &OscilloscopeWindow::OnFullscreen));
		m_btnFullscreen.set_tooltip_text("Fullscreen");
		m_btnFullscreen.set_label("Fullscreen");
		m_btnFullscreen.set_icon_widget(m_iconEnterFullscreen);
	m_toolbar.append(*Gtk::manage(new Gtk::SeparatorToolItem));

	m_toolbar.show_all();
}

/**
	@brief Creates the waveform areas for a new scope.
 */
void OscilloscopeWindow::CreateDefaultWaveformAreas(Gtk::Paned* split, bool nodigital, bool nospectrum)
{
	//Create top level waveform group
	auto defaultGroup = new WaveformGroup(this);
	m_waveformGroups.emplace(defaultGroup);
	split->pack1(defaultGroup->m_frame);

	//Create history windows
	for(auto scope : m_scopes)
		m_historyWindows[scope] = new HistoryWindow(this, scope);

	//Process all of the channels
	WaveformGroup* timeDomainGroup = NULL;
	WaveformGroup* frequencyDomainGroup = NULL;
	for(auto scope : m_scopes)
	{
		for(size_t i=0; i<scope->GetChannelCount(); i++)
		{
			auto chan = scope->GetChannel(i);

			//Qualify the channel name by the scope name if we have >1 scope enabled
			if(m_scopes.size() > 1)
				chan->SetDisplayName(scope->m_nickname + ":" + chan->GetHwname());

			auto type = chan->GetType();

			//Enable all channels to save time when setting up the client
			if( (type == OscilloscopeChannel::CHANNEL_TYPE_ANALOG) ||
				( (type == OscilloscopeChannel::CHANNEL_TYPE_DIGITAL) && !nodigital ) )
			{
				//Skip channels we can't enable
				if(!scope->CanEnableChannel(i))
					continue;

				//Put time and frequency domain channels in different groups
				bool freqDomain = chan->GetXAxisUnits() == Unit(Unit::UNIT_HZ);
				WaveformGroup* wg = NULL;
				if(freqDomain)
				{
					wg = frequencyDomainGroup;

					//Skip spectrum channels on request
					if(nospectrum)
						continue;
				}
				else
					wg = timeDomainGroup;

				//If the group doesn't exist yet, create/assign it
				if(wg == NULL)
				{
					//Both groups unassigned. Use default group for our current domain
					if( (timeDomainGroup == NULL) && (frequencyDomainGroup == NULL) )
						wg = defaultGroup;

					//Default group assigned, make a secondary one
					else
					{
						auto secondaryGroup = new WaveformGroup(this);
						m_waveformGroups.emplace(secondaryGroup);
						split->pack2(secondaryGroup->m_frame);
						wg = secondaryGroup;
					}

					//Either way, our domain now has a group
					if(freqDomain)
						frequencyDomainGroup = wg;
					else
						timeDomainGroup = wg;
				}

				//Create a waveform area for each stream in the output
				for(size_t j=0; j<chan->GetStreamCount(); j++)
				{
					//For now, assume all instrument channels have only one output stream
					auto w = new WaveformArea(StreamDescriptor(chan, j), this);
					w->m_group = wg;
					m_waveformAreas.emplace(w);
					if(type == OscilloscopeChannel::CHANNEL_TYPE_DIGITAL)
						wg->m_waveformBox.pack_start(*w, Gtk::PACK_SHRINK);
					else
						wg->m_waveformBox.pack_start(*w);
				}
			}
		}
	}

	//Done. Show everything except the measurement views
	show_all();
	if(frequencyDomainGroup)
		frequencyDomainGroup->m_measurementView.hide();
	if(timeDomainGroup)
		timeDomainGroup->m_measurementView.hide();
	defaultGroup->m_measurementView.hide();		//When starting up the application with no scope connected,
												//the default group is not yet committed to time or frequency domain.
												//So we have to hide the measurements regardless.
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Message handlers

bool OscilloscopeWindow::OnTimer(int /*timer*/)
{
	//Don't process any trigger events, etc during file load
	if(m_loadInProgress)
		return true;

	if(m_shuttingDown)
	{
		for(auto it : m_historyWindows)
			it.second->close();
		return false;
	}

	if(m_triggerArmed)
	{
		if(g_waveformReadyEvent.Peek())
		{
			//Clear old waveform timestamps for WFM/s display
			m_lastWaveformTimes.push_back(GetTime());
			while(m_lastWaveformTimes.size() > 10)
				m_lastWaveformTimes.erase(m_lastWaveformTimes.begin());

			//Crunch the new waveform
			{
				lock_guard<recursive_mutex> lock2(m_waveformDataMutex);

				//Update the history windows
				for(auto scope : m_scopes)
				{
					if(!scope->IsOffline())
						m_historyWindows[scope]->OnWaveformDataReady();
				}

				//Update filters etc once every instrument has been updated
				OnAllWaveformsUpdated(false, false);
			}

			//Release the waveform processing thread
			g_waveformProcessedEvent.Signal();

			//In multi-scope free-run mode, re-arm every instrument's trigger after we've processed all data
			if(m_multiScopeFreeRun)
				ArmTrigger(TRIGGER_TYPE_NORMAL);

			g_app->DispatchPendingEvents();
		}
	}

	//Discard all pending waveform data if the trigger isn't armed.
	//Failure to do this can lead to a spurious trigger after we wanted to stop.
	else
	{
		for(auto scope : m_scopes)
			scope->ClearPendingWaveforms();
	}

	//Clean up the scope sync wizard if it's completed
	if(m_syncComplete && (m_scopeSyncWizard != NULL) )
	{
		delete m_scopeSyncWizard;
		m_scopeSyncWizard = NULL;
	}

	return true;
}

void OscilloscopeWindow::OnPreferences()
{
    if(m_preferenceDialog)
        delete m_preferenceDialog;

    m_preferenceDialog = new PreferenceDialog{ this, m_preferences };
    m_preferenceDialog->show();
    m_preferenceDialog->signal_response().connect(sigc::mem_fun(*this, &OscilloscopeWindow::OnPreferenceDialogResponse));
}

/**
	@brief Update filter colors from the preferences manager
 */
void OscilloscopeWindow::SyncFilterColors()
{
	//Filter colors
	Filter::m_standardColors[Filter::COLOR_DATA] =
		m_preferences.GetColor("Appearance.Decodes.data_color");
	Filter::m_standardColors[Filter::COLOR_CONTROL] =
		m_preferences.GetColor("Appearance.Decodes.control_color");
	Filter::m_standardColors[Filter::COLOR_ADDRESS] =
		m_preferences.GetColor("Appearance.Decodes.address_color");
	Filter::m_standardColors[Filter::COLOR_PREAMBLE] =
		m_preferences.GetColor("Appearance.Decodes.preamble_color");
	Filter::m_standardColors[Filter::COLOR_CHECKSUM_OK] =
		m_preferences.GetColor("Appearance.Decodes.checksum_ok_color");
	Filter::m_standardColors[Filter::COLOR_CHECKSUM_BAD] =
		m_preferences.GetColor("Appearance.Decodes.checksum_bad_color");
	Filter::m_standardColors[Filter::COLOR_ERROR] =
		m_preferences.GetColor("Appearance.Decodes.error_color");
	Filter::m_standardColors[Filter::COLOR_IDLE] =
		m_preferences.GetColor("Appearance.Decodes.idle_color");

	//Protocol analyzer colors
	PacketDecoder::m_backgroundColors[PacketDecoder::PROTO_COLOR_DEFAULT] =
		m_preferences.GetColor("Appearance.Protocol Analyzer.default_color");
	PacketDecoder::m_backgroundColors[PacketDecoder::PROTO_COLOR_ERROR] =
		m_preferences.GetColor("Appearance.Protocol Analyzer.error_color");
	PacketDecoder::m_backgroundColors[PacketDecoder::PROTO_COLOR_STATUS] =
		m_preferences.GetColor("Appearance.Protocol Analyzer.status_color");
	PacketDecoder::m_backgroundColors[PacketDecoder::PROTO_COLOR_CONTROL] =
		m_preferences.GetColor("Appearance.Protocol Analyzer.control_color");
	PacketDecoder::m_backgroundColors[PacketDecoder::PROTO_COLOR_DATA_READ] =
		m_preferences.GetColor("Appearance.Protocol Analyzer.data_read_color");
	PacketDecoder::m_backgroundColors[PacketDecoder::PROTO_COLOR_DATA_WRITE] =
		m_preferences.GetColor("Appearance.Protocol Analyzer.data_write_color");
	PacketDecoder::m_backgroundColors[PacketDecoder::PROTO_COLOR_COMMAND] =
		m_preferences.GetColor("Appearance.Protocol Analyzer.command_color");
}

void OscilloscopeWindow::OnPreferenceDialogResponse(int response)
{
	if(response == Gtk::RESPONSE_OK)
	{
		m_preferenceDialog->SaveChanges();

		//Update the UI since we might have changed colors or other display settings
		SyncFilterColors();
		PopulateToolbar();
		SetTitle();
		for(auto w : m_waveformAreas)
		{
			w->SyncFontPreferences();
			w->queue_draw();
		}
		for(auto g : m_waveformGroups)
			g->m_timeline.queue_draw();
	}

	//Clean up the dialog
	delete m_preferenceDialog;
	m_preferenceDialog = NULL;
}

/**
	@brief Clean up when we're closed
 */
bool OscilloscopeWindow::on_delete_event(GdkEventAny* /*any_event*/)
{
	m_shuttingDown = true;

	CloseSession();
	return false;
}

/**
	@brief Shuts down the current session in preparation for opening a saved file etc
 */
void OscilloscopeWindow::CloseSession()
{
	lock_guard<recursive_mutex> lock(m_waveformDataMutex);

	//Stop the trigger so there's no pending waveforms
	OnStop();

	//Clear our trigger state
	//Important to signal the WaveformProcessingThread so it doesn't block waiting on response that's not going to come
	m_triggerArmed = false;
	g_waveformReadyEvent.Clear();
	g_waveformProcessedEvent.Signal();

    //Close popup dialogs, if they exist
    if(m_preferenceDialog)
    {
        m_preferenceDialog->hide();
        delete m_preferenceDialog;
        m_preferenceDialog = nullptr;
    }
    if(m_timebasePropertiesDialog)
    {
		m_timebasePropertiesDialog->hide();
		delete m_timebasePropertiesDialog;
		m_timebasePropertiesDialog = nullptr;
	}
	if(m_addFilterDialog)
    {
		m_addFilterDialog->hide();
		delete m_addFilterDialog;
		m_addFilterDialog = nullptr;
	}
	if(m_exportWizard)
	{
		m_exportWizard->hide();
		delete m_exportWizard;
		m_exportWizard = nullptr;
	}

    //Save preferences
    m_preferences.SavePreferences();

	//Need to clear the analyzers before we delete waveform areas.
	//Otherwise waveform areas will try to delete them too
	for(auto a : m_analyzers)
		delete a;
	m_analyzers.clear();

	//Close all of our UI elements
	for(auto it : m_historyWindows)
		delete it.second;
	for(auto s : m_splitters)
		delete s;
	for(auto g : m_waveformGroups)
		delete g;
	for(auto w : m_waveformAreas)
		delete w;
	for(auto it : m_meterDialogs)
		delete it.second;
	for(auto it : m_functionGeneratorDialogs)
		delete it.second;

	//Clear our records of them
	m_historyWindows.clear();
	m_splitters.clear();
	m_waveformGroups.clear();
	m_waveformAreas.clear();
	m_meterDialogs.clear();
	m_functionGeneratorDialogs.clear();

	delete m_scopeSyncWizard;
	m_scopeSyncWizard = NULL;

	delete m_graphEditor;
	m_graphEditor = NULL;

	m_multiScopeFreeRun = false;

	//Delete stuff from our UI
	auto children = m_setupTriggerMenu.get_children();
	for(auto c : children)
		m_setupTriggerMenu.remove(*c);

	//Close stuff in the application, terminate threads, etc
	g_app->ShutDownSession();

	//Get rid of function generators
	//(but only delete them if they're not also a scope)
	for(auto gen : m_funcgens)
	{
		if(0 == (gen->GetInstrumentTypes() & Instrument::INST_OSCILLOSCOPE) )
			delete gen;
	}
	m_funcgens.clear();

	//Get rid of scopes
	for(auto scope : m_scopes)
		delete scope;
	m_scopes.clear();

	SetTitle();
}

/**
	@brief Import waveform data not in the native glscopeclient format
 */
void OscilloscopeWindow::OnFileImport()
{
	//TODO: prompt to save changes to the current session
	Gtk::FileChooserDialog dlg(*this, "Import", Gtk::FILE_CHOOSER_ACTION_OPEN);

	string binname = "Agilent/Keysight/Rigol Binary Capture (*.bin)";

	auto binFilter = Gtk::FileFilter::create();
	binFilter->add_pattern("*.bin");
	binFilter->set_name(binname);

	dlg.add_filter(binFilter);
	dlg.add_button("Open", Gtk::RESPONSE_OK);
	dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
	auto response = dlg.run();

	if(response != Gtk::RESPONSE_OK)
		return;

	lock_guard<recursive_mutex> lock(m_waveformDataMutex);

	auto filterName = dlg.get_filter()->get_name();
	if(filterName == binname)
		DoImportBIN(dlg.get_filename());
}

/**
	@brief Create a new session for importing a file into
 */
MockOscilloscope* OscilloscopeWindow::SetupNewSessionForImport(const string& name, const string& filename)
{
	lock_guard<recursive_mutex> lock(m_waveformDataMutex);

	//Setup
	CloseSession();
	m_currentFileName = filename;
	m_loadInProgress = true;

	//Clear performance counters
	m_totalWaveforms = 0;
	m_lastWaveformTimes.clear();

	//Create the mock scope
	auto scope = new MockOscilloscope(name, "Generic", "12345");
	scope->m_nickname = "import";
	m_scopes.push_back(scope);

	//Set up history for it
	auto hist = new HistoryWindow(this, scope);
	hist->hide();
	m_historyWindows[scope] = hist;

	return scope;
}

/**
	@brief Sets up an existing session for importing a file into
 */
MockOscilloscope* OscilloscopeWindow::SetupExistingSessionForImport()
{
	lock_guard<recursive_mutex> lock(m_waveformDataMutex);

	auto scope = dynamic_cast<MockOscilloscope*>(m_scopes[0]);
	if(scope == NULL)
	{
		LogError("not a mock scope, can't import anything into it\n");
		return NULL;
	}

	//TODO: proper timestamp?
	m_lastWaveformTimes.push_back(GetTime());
	while(m_lastWaveformTimes.size() > 10)
		m_lastWaveformTimes.erase(m_lastWaveformTimes.begin());

	//Detach the old waveform data so we don't destroy it
	for(size_t i=0; i<scope->GetChannelCount(); i++)
	{
		auto chan = scope->GetChannel(i);
		for(size_t j=0; j<chan->GetStreamCount(); j++)
			chan->Detach(j);
	}

	return scope;
}

/**
	@brief Sets up default viewports etc upon completion of an import
 */
void OscilloscopeWindow::OnImportComplete()
{
	//Add the top level splitter right before the status bar
	auto split = new Gtk::VPaned;
	m_splitters.emplace(split);
	m_vbox.remove(m_statusbar);
	m_vbox.pack_start(*split, Gtk::PACK_EXPAND_WIDGET);
	m_vbox.pack_start(m_statusbar, Gtk::PACK_SHRINK);

	//Add all of the UI stuff
	CreateDefaultWaveformAreas(split);

	//Done
	SetTitle();
	OnLoadComplete();

	//Process the new data
	m_historyWindows[m_scopes[0]]->OnWaveformDataReady();
	OnAllWaveformsUpdated();
}

/**
	@brief Import a Agilent/Keysight BIN file
 */
void OscilloscopeWindow::DoImportBIN(const string& filename)
{
	lock_guard<recursive_mutex> lock(m_waveformDataMutex);

	LogDebug("Importing BIN file \"%s\"\n", filename.c_str());
	{
		LogIndenter li;

		auto scope = SetupNewSessionForImport("Binary Import", filename);

		//Load the waveform
		if(!scope->LoadBIN(filename))
		{
			Gtk::MessageDialog dlg(
				*this,
				"BIN import failed",
				false,
				Gtk::MESSAGE_ERROR,
				Gtk::BUTTONS_OK,
				true);
			dlg.run();
		}
	}

	OnImportComplete();
}

/**
	@brief Connect to an instrument
 */
void OscilloscopeWindow::OnFileConnect()
{
	//TODO: support multi-scope connection
	InstrumentConnectionDialog dlg;
	while(true)
	{
		if(dlg.run() != Gtk::RESPONSE_OK)
			return;

		//If the user requested an illegal configuration, retry
		if(!dlg.ValidateConfig())
		{
			Gtk::MessageDialog mdlg(
				"Invalid configuration specified.\n"
				"\n"
				"A driver and transport must always be selected.\n"
				"\n"
				"The NULL transport is only legal with the \"demo\" driver.",
				false,
				Gtk::MESSAGE_ERROR,
				Gtk::BUTTONS_OK,
				true);
			mdlg.run();
		}

		else
			break;
	}

	ConnectToScope(dlg.GetConnectionString());
}

void OscilloscopeWindow::ConnectToScope(string path)
{
	vector<string> scopes;
	scopes.push_back(path);

	//Connect to the new scope
	CloseSession();
	m_loadInProgress = true;
	m_scopes = g_app->ConnectToScopes(scopes);

	//Clear performance counters
	m_totalWaveforms = 0;
	m_lastWaveformTimes.clear();

	//Add the top level splitter right before the status bar
	auto split = new Gtk::VPaned;
	m_splitters.emplace(split);
	m_vbox.remove(m_statusbar);
	m_vbox.pack_start(*split, Gtk::PACK_EXPAND_WIDGET);
	m_vbox.pack_start(m_statusbar, Gtk::PACK_SHRINK);

	//Add all of the UI stuff
	CreateDefaultWaveformAreas(split);

	//Done
	SetTitle();
	OnLoadComplete();

	//Arm the trigger
	OnStart();
}

/**
	@brief Open a saved configuration
 */
void OscilloscopeWindow::OnFileOpen()
{
	//TODO: prompt to save changes to the current session

	Gtk::FileChooserDialog dlg(*this, "Open", Gtk::FILE_CHOOSER_ACTION_OPEN);

	dlg.add_choice("layout", "Load UI Configuration");
	dlg.add_choice("waveform", "Load Waveform Data");
	dlg.add_choice("reconnect", "Reconnect to Instrument (reconfigure using saved settings)");

	dlg.set_choice("layout", "true");
	dlg.set_choice("waveform", "true");
	dlg.set_choice("reconnect", "true");

	auto filter = Gtk::FileFilter::create();
	filter->add_pattern("*.scopesession");
	filter->set_name("glscopeclient sessions (*.scopesession)");
	dlg.add_filter(filter);
	dlg.add_button("Open", Gtk::RESPONSE_OK);
	dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
	auto response = dlg.run();

	if(response != Gtk::RESPONSE_OK)
		return;

	bool loadLayout = dlg.get_choice("layout") == "true";
	bool loadWaveform = dlg.get_choice("waveform") == "true";
	bool reconnect = dlg.get_choice("reconnect") == "true";
	DoFileOpen(dlg.get_filename(), loadLayout, loadWaveform, reconnect);
}

/**
	@brief Open a saved file
 */
void OscilloscopeWindow::DoFileOpen(const string& filename, bool loadLayout, bool loadWaveform, bool reconnect)
{
	lock_guard<recursive_mutex> lock(m_waveformDataMutex);

	m_currentFileName = filename;

	m_loadInProgress = true;

	CloseSession();

	//Clear performance counters
	m_totalWaveforms = 0;
	m_lastWaveformTimes.clear();

	try
	{
		auto docs = YAML::LoadAllFromFile(m_currentFileName);

		//Only open the first doc, our file format doesn't ever generate multiple docs in a file.
		//Ignore any trailing stuff at the end
		auto node = docs[0];

		//Load various sections of the file
		IDTable table;
		LoadInstruments(node["instruments"], reconnect, table);
		if(loadLayout)
		{
			LoadDecodes(node["decodes"], table);
			LoadUIConfiguration(node["ui_config"], table);
		}

		//Create history windows for all of our scopes
		for(auto scope : m_scopes)
		{
			auto hist = new HistoryWindow(this, scope);
			hist->hide();
			m_historyWindows[scope] = hist;
		}

		//Re-title the window for the new scope
		SetTitle();

		//Load data
		try
		{
			if(loadWaveform)
				LoadWaveformData(filename, table);
		}
		catch(const YAML::BadFile& ex)
		{
			Gtk::MessageDialog dlg(
				*this,
				"Failed to load saved waveform data",
				false,
				Gtk::MESSAGE_ERROR,
				Gtk::BUTTONS_OK,
				true);
			dlg.run();
		}
	}
	catch(const YAML::BadFile& ex)
	{
		Gtk::MessageDialog dlg(
			*this,
			string("Unable to open file ") + filename + ".",
			false,
			Gtk::MESSAGE_ERROR,
			Gtk::BUTTONS_OK,
			true);
		dlg.run();
		return;
	}

	OnLoadComplete();
}

/**
	@brief Refresh everything in the UI when a new file has been loaded
 */
void OscilloscopeWindow::OnLoadComplete()
{
	FindScopeFuncGens();

	//TODO: refresh measurements and protocol decodes

	//Create protocol analyzers
	for(auto area : m_waveformAreas)
	{
		for(size_t i=0; i<area->GetOverlayCount(); i++)
		{
			auto pdecode = dynamic_cast<PacketDecoder*>(area->GetOverlay(i).m_channel);
			if(pdecode != NULL)
			{
				char title[256];
				snprintf(title, sizeof(title), "Protocol Analyzer: %s", pdecode->GetDisplayName().c_str());

				auto analyzer = new ProtocolAnalyzerWindow(title, this, pdecode, area);
				m_analyzers.emplace(analyzer);

				//Done
				analyzer->show();
			}
		}
	}

	//Reconfigure menus
	AddCurrentToRecentlyUsedList();
	SaveRecentlyUsedList();
	RefreshInstrumentMenu();
	RefreshChannelsMenu();
	RefreshAnalyzerMenu();
	RefreshMultimeterMenu();
	RefreshTriggerMenu();
	RefreshGeneratorsMenu();

	//Make sure all resize etc events have been handled before replaying history.
	//Otherwise eye patterns don't refresh right.
	show_all();
	GarbageCollectGroups();
	g_app->DispatchPendingEvents();

	//TODO: make this work properly if we have decodes spanning multiple scopes
	for(auto it : m_historyWindows)
		it.second->ReplayHistory();

	//Filters are refreshed by ReplayHistory(), but if we have no scopes (all waveforms created by filters)
	//then nothing will happen. In this case, a manual refresh of the filter graph is necessary.
	if(m_scopes.empty())
		RefreshAllFilters();

	//Start threads to poll scopes etc
	else
		g_app->StartScopeThreads(m_scopes);

	//Done loading, we can render everything for good now.
	//Issue 2 render calls since the very first render does some setup stuff
	m_loadInProgress = false;
	ClearAllPersistence();
	g_app->DispatchPendingEvents();
	ClearAllPersistence();
}

/**
	@brief Loads waveform data for a save file
 */
void OscilloscopeWindow::LoadWaveformData(string filename, IDTable& table)
{
	//Create and show progress dialog
	FileProgressDialog progress;
	progress.show();

	//Figure out data directory
	string base = filename.substr(0, filename.length() - strlen(".scopesession"));
	string datadir = base + "_data";

	//Load data for each scope
	float progress_per_scope = 1.0f / m_scopes.size();
	for(size_t i=0; i<m_scopes.size(); i++)
	{
		auto scope = m_scopes[i];
		int id = table[scope];

		char tmp[512];
		snprintf(tmp, sizeof(tmp), "%s/scope_%d_metadata.yml", datadir.c_str(), id);
		auto docs = YAML::LoadAllFromFile(tmp);

		LoadWaveformDataForScope(docs[0], scope, datadir, table, progress, i*progress_per_scope, progress_per_scope);
	}
}

/**
	@brief Loads waveform data for a single instrument
 */
void OscilloscopeWindow::LoadWaveformDataForScope(
	const YAML::Node& node,
	Oscilloscope* scope,
	string datadir,
	IDTable& table,
	FileProgressDialog& progress,
	float base_progress,
	float progress_range
	)
{
	progress.Update("Loading oscilloscope configuration", base_progress);

	TimePoint time;
	time.first = 0;
	time.second = 0;

	TimePoint newest;
	newest.first = 0;
	newest.second = 0;

	auto window = m_historyWindows[scope];
	int scope_id = table[scope];

	//Clear out any old waveforms the instrument may have
	for(size_t i=0; i<scope->GetChannelCount(); i++)
	{
		auto chan = scope->GetChannel(i);
		for(size_t j=0; j<chan->GetStreamCount(); j++)
			chan->SetData(NULL, j);
	}

	//Preallocate size
	auto wavenode = node["waveforms"];
	window->SetMaxWaveforms(wavenode.size());

	//Load the data for each waveform
	float waveform_progress = progress_range / wavenode.size();
	size_t iwave = 0;
	for(auto it : wavenode)
	{
		iwave ++;

		//Top level metadata
		bool timebase_is_ps = true;
		auto wfm = it.second;
		time.first = wfm["timestamp"].as<long long>();
		if(wfm["time_psec"])
		{
			time.second = wfm["time_psec"].as<long long>() * 1000;
			timebase_is_ps = true;
		}
		else
		{
			time.second = wfm["time_fsec"].as<long long>();
			timebase_is_ps = false;
		}
		int waveform_id = wfm["id"].as<int>();

		//Set up channel metadata first (serialized)
		auto chans = wfm["channels"];
		vector<pair<int, int>> channels;	//pair<channel, stream>
		vector<string> formats;
		for(auto jt : chans)
		{
			auto ch = jt.second;
			int channel_index = ch["index"].as<int>();
			int stream = 0;
			if(ch["stream"])
				stream = ch["stream"].as<int>();
			auto chan = scope->GetChannel(channel_index);
			channels.push_back(pair<int, int>(channel_index, stream));

			//Waveform format defaults to sparsev1 as that's what was used before
			//the metadata file contained a format ID at all
			string format = "sparsev1";
			if(ch["format"])
				format = ch["format"].as<string>();
			formats.push_back(format);

			//TODO: support non-analog/digital captures (eyes, spectrograms, etc)
			WaveformBase* cap = NULL;
			AnalogWaveform* acap = NULL;
			DigitalWaveform* dcap = NULL;
			if(chan->GetType() == OscilloscopeChannel::CHANNEL_TYPE_ANALOG)
				cap = acap = new AnalogWaveform;
			else
				cap = dcap = new DigitalWaveform;

			//Channel waveform metadata
			cap->m_timescale = ch["timescale"].as<long>();
			cap->m_startTimestamp = time.first;
			cap->m_startFemtoseconds = time.second;
			if(timebase_is_ps)
			{
				cap->m_timescale *= 1000;
				cap->m_triggerPhase = ch["trigphase"].as<float>() * 1000;
			}
			else
				cap->m_triggerPhase = ch["trigphase"].as<long long>();

			chan->Detach(stream);
			chan->SetData(cap, stream);
		}

		//Kick off a thread to load data for each channel
		vector<thread*> threads;
		size_t nchans = channels.size();
		volatile float* channel_progress = new float[nchans];
		volatile int* channel_done = new int[nchans];
		for(size_t i=0; i<channels.size(); i++)
		{
			channel_progress[i] = 0;
			channel_done[i] = 0;

			threads.push_back(new thread(
				&OscilloscopeWindow::DoLoadWaveformDataForScope,
				channels[i].first,
				channels[i].second,
				scope,
				datadir,
				scope_id,
				waveform_id,
				formats[i],
				channel_progress + i,
				channel_done + i
				));
		}

		//Process events and update the display with each thread's progress
		while(true)
		{
			//Figure out total progress across each channel. Stop if all threads are done
			bool done = true;
			float frac = 0;
			for(size_t i=0; i<nchans; i++)
			{
				if(!channel_done[i])
					done = false;
				frac += channel_progress[i];
			}
			if(done)
				break;
			frac /= nchans;

			//Update the UI
			char tmp[256];
			snprintf(
				tmp,
				sizeof(tmp),
				"Loading waveform %zu/%zu for instrument %s: %.0f %% complete",
				iwave,
				wavenode.size(),
				scope->m_nickname.c_str(),
				frac * 100);
			progress.Update(tmp, base_progress + frac*waveform_progress);
			std::this_thread::sleep_for(std::chrono::microseconds(1000 * 50));

			g_app->DispatchPendingEvents();
		}

		delete[] channel_progress;
		delete[] channel_done;

		//Wait for threads to complete
		for(auto t : threads)
		{
			t->join();
			delete t;
		}

		//Add to history
		window->OnWaveformDataReady(true);

		//Keep track of the newest waveform (may not be in time order)
		if( (time.first > newest.first) ||
			( (time.first == newest.first) &&  (time.second > newest.second) ) )
		{
			newest = time;
		}

		base_progress += waveform_progress;
	}

	window->JumpToHistory(newest);
}

void OscilloscopeWindow::DoLoadWaveformDataForScope(
	int channel_index,
	int stream,
	Oscilloscope* scope,
	string datadir,
	int scope_id,
	int waveform_id,
	string format,
	volatile float* progress,
	volatile int* done
	)
{
	auto chan = scope->GetChannel(channel_index);

	auto cap = chan->GetData(stream);
	auto acap = dynamic_cast<AnalogWaveform*>(cap);
	auto dcap = dynamic_cast<DigitalWaveform*>(cap);

	//Load the actual sample data
	char tmp[512];
	if(stream == 0)
	{
		snprintf(tmp, sizeof(tmp), "%s/scope_%d_waveforms/waveform_%d/channel_%d.bin",
			datadir.c_str(),
			scope_id,
			waveform_id,
			channel_index);
	}
	else
	{
		snprintf(tmp, sizeof(tmp), "%s/scope_%d_waveforms/waveform_%d/channel_%d_stream%d.bin",
			datadir.c_str(),
			scope_id,
			waveform_id,
			channel_index,
			stream);
	}

	//Load samples into memory
	unsigned char* buf = NULL;

	//Windows: use generic file reads for now
	#ifdef _WIN32
		FILE* fp = fopen(tmp, "rb");
		if(!fp)
		{
			LogError("couldn't open %s\n", tmp);
			return;
		}

		//Read the whole file into a buffer a megabyte at a time
		fseek(fp, 0, SEEK_END);
		long len = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		buf = new unsigned char[len];
		long len_remaining = len;
		long blocksize = 1024*1024;
		long read_offset = 0;
		while(len_remaining > 0)
		{
			if(blocksize > len_remaining)
				blocksize = len_remaining;

			//Most time is spent on the fread's when using this path
			*progress = read_offset * 1.0 / len;
			fread(buf + read_offset, 1, blocksize, fp);

			len_remaining -= blocksize;
			read_offset += blocksize;
		}
		fclose(fp);

	//On POSIX, just memory map the file
	#else
		int fd = open(tmp, O_RDONLY);
		if(fd < 0)
		{
			LogError("couldn't open %s\n", tmp);
			return;
		}
		size_t len = lseek(fd, 0, SEEK_END);
		buf = (unsigned char*)mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);

		//For now, report progress complete upon the file being fully read
		*progress = 1;
	#endif

	//Sparse interleaved
	if(format == "sparsev1")
	{
		//Figure out how many samples we have
		size_t samplesize = 2*sizeof(int64_t);
		if(acap)
			samplesize += sizeof(float);
		else
			samplesize += sizeof(bool);
		size_t nsamples = len / samplesize;
		cap->Resize(nsamples);

		//TODO: AVX this?
		for(size_t j=0; j<nsamples; j++)
		{
			size_t offset = j*samplesize;

			//Read start time and duration
			int64_t* stime = reinterpret_cast<int64_t*>(buf+offset);
			offset += 2*sizeof(int64_t);
			cap->m_offsets[j] = stime[0];
			cap->m_durations[j] = stime[1];

			//Read sample data
			if(acap)
			{
				//The file format assumes "float" is IEEE754 32-bit float.
				//If your platform doesn't do that, good luck.
				//cppcheck-suppress invalidPointerCast
				acap->m_samples[j] = *reinterpret_cast<float*>(buf+offset);
			}

			else
				dcap->m_samples[j] = *reinterpret_cast<bool*>(buf+offset);

			//TODO: progress updates
		}

		//Quickly check if the waveform is dense packed, even if it was stored as sparse.
		//Since we know samples must be monotonic and non-overlapping, we don't have to check every single one!
		int64_t nlast = nsamples - 1;
		if( (cap->m_offsets[0] == 0) &&
			(cap->m_offsets[nlast] == nlast) &&
			(cap->m_durations[nlast] == 1) )
		{
			cap->m_densePacked = true;
		}
	}

	//Dense packed
	else if(format == "densev1")
	{
		cap->m_densePacked = true;

		//Figure out length
		size_t nsamples = 0;
		if(acap)
			nsamples = len / sizeof(float);
		else if(dcap)
			nsamples = len / sizeof(bool);
		cap->Resize(nsamples);

		//Read sample data
		if(acap)
			memcpy(&acap->m_samples[0], buf, nsamples*sizeof(float));
		else
			memcpy(&dcap->m_samples[0], buf, nsamples*sizeof(bool));

		//TODO: vectorized initialization of timestamps and durations
		for(size_t i=0; i<nsamples; i++)
		{
			cap->m_offsets[i] = i;
			cap->m_durations[i] = 1;
		}
	}

	else
	{
		LogError(
			"Unknown waveform format \"%s\", perhaps this file was created by a newer version of glscopeclient?\n",
			format.c_str());
	}

	#ifdef _WIN32
		delete[] buf;
	#else
		munmap(buf, len);
		::close(fd);
	#endif

	*done = 1;
	*progress = 1;
}

/**
	@brief Reconnect to existing instruments and reconfigure them
 */
void OscilloscopeWindow::LoadInstruments(const YAML::Node& node, bool reconnect, IDTable& table)
{
	if(!node)
	{
		LogError("Save file missing instruments node\n");
		return;
	}

	//Load each instrument
	for(auto it : node)
	{
		auto inst = it.second;

		Oscilloscope* scope = NULL;

		auto transtype = inst["transport"].as<string>();
		auto driver = inst["driver"].as<string>();

		if(reconnect)
		{
			if( (transtype == "null") && (driver != "demo") )
			{
				Gtk::MessageDialog dlg(
					*this,
					"Cannot reconnect to instrument because the .scopesession file does not contain any connection "
					"information.\n\n"
					"Loading file in offline mode.",
					false,
					Gtk::MESSAGE_ERROR,
					Gtk::BUTTONS_OK,
					true);
				dlg.run();
			}
			else
			{
				//Create the scope
				auto transport = SCPITransport::CreateTransport(transtype, inst["args"].as<string>());

				//Check if the transport failed to initialize
				if((transport == NULL) || !transport->IsConnected())
				{
					Gtk::MessageDialog dlg(
						*this,
						string("Failed to connect to instrument using connection string ") + inst["args"].as<string>(),
						false,
						Gtk::MESSAGE_ERROR,
						Gtk::BUTTONS_OK,
						true);
					dlg.run();
				}

				//All good, try to connect
				else
				{
					scope = Oscilloscope::CreateOscilloscope(driver, transport);

					//Sanity check make/model/serial. If mismatch, stop
					string message;
					bool fail = false;
					if(inst["name"].as<string>() != scope->GetName())
					{
						message = string("Unable to connect to oscilloscope: instrument has model name \"") +
							scope->GetName() + "\", save file has model name \"" + inst["name"].as<string>()  + "\"";
						fail = true;
					}
					else if(inst["vendor"].as<string>() != scope->GetVendor())
					{
						message = string("Unable to connect to oscilloscope: instrument has vendor \"") +
							scope->GetVendor() + "\", save file has vendor \"" + inst["vendor"].as<string>()  + "\"";
						fail = true;
					}
					else if(inst["serial"].as<string>() != scope->GetSerial())
					{
						message = string("Unable to connect to oscilloscope: instrument has serial \"") +
							scope->GetSerial() + "\", save file has serial \"" + inst["serial"].as<string>()  + "\"";
						fail = true;
					}
					if(fail)
					{
						Gtk::MessageDialog dlg(*this, message, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
						dlg.run();
						delete scope;
						scope = NULL;
					}
				}
			}
		}

		if(!scope)
		{
			//Create the mock scope
			scope = new MockOscilloscope(
				inst["name"].as<string>(),
				inst["vendor"].as<string>(),
				inst["serial"].as<string>());
		}

		//All good. Add to our list of scopes etc
		m_scopes.push_back(scope);
		table.emplace(inst["id"].as<int>(), scope);

		//Configure the scope
		scope->LoadConfiguration(inst, table);
	}
}

/**
	@brief Load protocol decoder configuration
 */
void OscilloscopeWindow::LoadDecodes(const YAML::Node& node, IDTable& table)
{
	//No protocol decodes? Skip this section
	if(!node)
		return;

	//Load each decode
	for(auto it : node)
	{
		auto dnode = it.second;

		//Create the decode
		auto proto = dnode["protocol"].as<string>();
		auto filter = Filter::CreateFilter(proto, dnode["color"].as<string>());
		if(filter == NULL)
		{
			Gtk::MessageDialog dlg(
				string("Unable to create filter \"") + proto + "\". Skipping...\n",
				false,
				Gtk::MESSAGE_ERROR,
				Gtk::BUTTONS_OK,
				true);
			dlg.run();
			continue;
		}

		table.emplace(dnode["id"].as<int>(), filter);

		//Load parameters during the first pass.
		//Parameters can't have dependencies on other channels etc.
		//More importantly, parameters may change bus width etc
		filter->LoadParameters(dnode, table);
	}

	//Make a second pass to configure the filter inputs, once all of them have been instantiated.
	//Filters may depend on other filters as inputs, and serialization is not guaranteed to be a topological sort.
	for(auto it : node)
	{
		auto dnode = it.second;
		auto filter = static_cast<Filter*>(table[dnode["id"].as<int>()]);
		if(filter)
			filter->LoadInputs(dnode, table);
	}
}

/**
	@brief Load user interface configuration
 */
void OscilloscopeWindow::LoadUIConfiguration(const YAML::Node& node, IDTable& table)
{
	//Window configuration
	auto wnode = node["window"];
	resize(wnode["width"].as<int>(), wnode["height"].as<int>());

	//Waveform areas
	auto areas = node["areas"];
	for(auto it : areas)
	{
		//Load the area itself
		auto an = it.second;
		auto channel = static_cast<OscilloscopeChannel*>(table[an["channel"].as<int>()]);
		if(!channel)	//don't crash on bad IDs or missing decodes
			continue;
		size_t stream = 0;
		if(an["stream"])
			stream = an["stream"].as<int>();
		WaveformArea* area = new WaveformArea(StreamDescriptor(channel, stream), this);
		table.emplace(an["id"].as<int>(), area);
		area->SetPersistenceEnabled(an["persistence"].as<int>() ? true : false);
		m_waveformAreas.emplace(area);

		//Add any overlays
		auto overlays = an["overlays"];
		for(auto jt : overlays)
		{
			auto filter = static_cast<Filter*>(table[jt.second["id"].as<int>()]);
			stream = 0;
			if(jt.second["stream"])
				stream = jt.second["stream"].as<int>();
			if(filter)
				area->AddOverlay(StreamDescriptor(filter, stream));
		}
	}

	//Waveform groups
	auto groups = node["groups"];
	for(auto it : groups)
	{
		//Create the group
		auto gn = it.second;
		WaveformGroup* group = new WaveformGroup(this);
		table.emplace(gn["id"].as<int>(), &group->m_frame);
		group->m_framelabel.set_label(gn["name"].as<string>());

		//Scale if needed
		bool timestamps_are_ps = true;
		if(gn["timebaseResolution"])
		{
			if(gn["timebaseResolution"].as<string>() == "fs")
				timestamps_are_ps = false;
		}

		group->m_pixelsPerXUnit = gn["pixelsPerXUnit"].as<float>();
		group->m_xAxisOffset = gn["xAxisOffset"].as<long>();
		m_waveformGroups.emplace(group);

		//Cursor config
		string cursor = gn["cursorConfig"].as<string>();
		if(cursor == "none")
			group->m_cursorConfig = WaveformGroup::CURSOR_NONE;
		else if(cursor == "x_single")
			group->m_cursorConfig = WaveformGroup::CURSOR_X_SINGLE;
		else if(cursor == "x_dual")
			group->m_cursorConfig = WaveformGroup::CURSOR_X_DUAL;
		else if(cursor == "y_single")
			group->m_cursorConfig = WaveformGroup::CURSOR_Y_SINGLE;
		else if(cursor == "y_dual")
			group->m_cursorConfig = WaveformGroup::CURSOR_Y_DUAL;
		group->m_xCursorPos[0] = gn["xcursor0"].as<long>();
		group->m_xCursorPos[1] = gn["xcursor1"].as<long>();
		group->m_yCursorPos[0] = gn["ycursor0"].as<float>();
		group->m_yCursorPos[1] = gn["ycursor1"].as<float>();

		if(timestamps_are_ps)
		{
			group->m_pixelsPerXUnit /= 1000;
			group->m_xAxisOffset *= 1000;
			group->m_xCursorPos[0] *= 1000;
			group->m_xCursorPos[1] *= 1000;
		}

		auto stats = gn["stats"];
		if(stats)
		{
			for(auto s : stats)
			{
				auto statnode = s.second;
				int stream = 0;
				if(statnode["stream"])
					stream = statnode["stream"].as<long>();

				group->EnableStats(
					StreamDescriptor(
						static_cast<OscilloscopeChannel*>(table[statnode["channel"].as<long>()]),
						stream),
					statnode["index"].as<long>());
			}
		}

		//Waveform areas
		areas = gn["areas"];
		for(auto at : areas)
		{
			auto area = static_cast<WaveformArea*>(table[at.second["id"].as<int>()]);
			if(!area)
				continue;
			area->m_group = group;
			if(area->GetChannel().m_channel->GetType() == OscilloscopeChannel::CHANNEL_TYPE_DIGITAL)
				group->m_waveformBox.pack_start(*area, Gtk::PACK_SHRINK);
			else
				group->m_waveformBox.pack_start(*area);
		}
	}

	//Splitters
	auto splitters = node["splitters"];
	for(auto it : splitters)
	{
		//Create the splitter
		auto sn = it.second;
		auto dir = sn["dir"].as<string>();
		Gtk::Paned* split = NULL;
		if(dir == "h")
			split = new Gtk::HPaned;
		else
			split = new Gtk::VPaned;
		m_splitters.emplace(split);
		table.emplace(sn["id"].as<int>(), split);
	}
	for(auto it : splitters)
	{
		auto sn = it.second;
		Gtk::Paned* split = static_cast<Gtk::Paned*>(table[sn["id"].as<int>()]);

		auto a = static_cast<Gtk::Widget*>(table[sn["child0"].as<int>()]);
		auto b = static_cast<Gtk::Widget*>(table[sn["child1"].as<int>()]);
		if(a)
			split->pack1(*a);
		if(b)
			split->pack2(*b);
		split->set_position(sn["split"].as<int>());
	}

	//Add the top level splitter right before the status bar
	m_vbox.remove(m_statusbar);
	m_vbox.pack_start(*static_cast<Gtk::Paned*>(table[node["top"].as<int>()]), Gtk::PACK_EXPAND_WIDGET);
	m_vbox.pack_start(m_statusbar, Gtk::PACK_SHRINK);
}

/**
	@brief Common handler for save/save as commands
 */
void OscilloscopeWindow::OnFileSave(bool saveToCurrentFile, bool saveLayout, bool saveWaveforms)
{
	bool creatingNew = false;

	static const char* extension = ".scopesession";

	//Pop up the dialog if we asked for a new file.
	//But if we don't have a current file, we need to prompt regardless
	if(m_currentFileName.empty() || !saveToCurrentFile)
	{
		creatingNew = true;

		string title = "Save ";
		if(saveLayout)
		{
			title += "Layout";
			if(saveWaveforms)
				title += " and ";
		}
		if(saveWaveforms)
			title += "Waveforms";

		Gtk::FileChooserDialog dlg(*this, title, Gtk::FILE_CHOOSER_ACTION_SAVE);

		auto filter = Gtk::FileFilter::create();
		filter->add_pattern("*.scopesession");
		filter->set_name("glscopeclient sessions (*.scopesession)");
		dlg.add_filter(filter);
		dlg.add_button("Save", Gtk::RESPONSE_OK);
		dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
		dlg.set_uri(m_currentFileName);
		dlg.set_do_overwrite_confirmation();
		auto response = dlg.run();

		if(response != Gtk::RESPONSE_OK)
			return;

		m_currentFileName = dlg.get_filename();
	}

	//Add the extension if not present
	if(m_currentFileName.find(extension) == string::npos)
		m_currentFileName += extension;

	//Format the directory name
	m_currentDataDirName = m_currentFileName.substr(0, m_currentFileName.length() - strlen(extension)) + "_data";

	//See if the directory exists
	bool dir_exists = false;

#ifndef _WIN32
	int hfile = open(m_currentDataDirName.c_str(), O_RDONLY);
	if(hfile >= 0)
	{
		//It exists as a file. Reopen and check if it's a directory
		::close(hfile);
		hfile = open(m_currentDataDirName.c_str(), O_RDONLY | O_DIRECTORY);

		//If this open works, it's a directory.
		if(hfile >= 0)
		{
			::close(hfile);
			dir_exists = true;
		}

		//Data dir exists, but it's something else! Error out
		else
		{
			string msg = string("The data directory ") + m_currentDataDirName + " already exists, but is not a directory!";
			Gtk::MessageDialog errdlg(msg, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
			errdlg.set_title("Cannot save session\n");
			errdlg.run();
			return;
		}
	}
#else
	auto fileType = GetFileAttributes(m_currentDataDirName.c_str());

	// Check if any file exists at this path
	if(fileType != INVALID_FILE_ATTRIBUTES)
	{
		if(fileType & FILE_ATTRIBUTE_DIRECTORY)
		{
			// directory exists
			dir_exists = true;
		}
		else
		{
			// Its some other file
			string msg = string("The data directory ") + m_currentDataDirName + " already exists, but is not a directory!";
			Gtk::MessageDialog errdlg(msg, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
			errdlg.set_title("Cannot save session\n");
			errdlg.run();
			return;
		}
	}
#endif

	//See if the file exists
	bool file_exists = false;

#ifndef _WIN32
	hfile = open(m_currentFileName.c_str(), O_RDONLY);
	if(hfile >= 0)
	{
		file_exists = true;
		::close(hfile);
	}
#else
	auto fileAttr = GetFileAttributes(m_currentFileName.c_str());

	file_exists = (fileAttr != INVALID_FILE_ATTRIBUTES
		&& !(fileAttr & FILE_ATTRIBUTE_DIRECTORY));

#endif


	//If we are trying to create a new file, warn if the directory exists but the file does not
	//If the file exists GTK will warn, and we don't want to prompt the user twice if both exist!
	if(creatingNew && (dir_exists && !file_exists))
	{
		string msg = string("The data directory ") + m_currentDataDirName +
			" already exists. Overwrite existing contents?";
		Gtk::MessageDialog errdlg(msg, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_YES_NO, true);
		errdlg.set_title("Save session\n");
		if(errdlg.run() != Gtk::RESPONSE_YES)
			return;
	}

	//Create the directory we're saving to (if needed)
	if(!dir_exists)
	{
#ifdef _WIN32
		auto result = mkdir(m_currentDataDirName.c_str());
#else
		auto result = mkdir(m_currentDataDirName.c_str(), 0755);
#endif

		if(0 != result)
		{
			string msg = string("The data directory ") + m_currentDataDirName + " could not be created!";
			Gtk::MessageDialog errdlg(msg, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
			errdlg.set_title("Cannot save session\n");
			errdlg.run();
			return;
		}
	}

	//If we're currently capturing, stop.
	//This prevents waveforms from changing under our nose as we're serializing.
	OnStop();

	//Serialize our configuration and save to the file
	IDTable table;
	string config = SerializeConfiguration(saveLayout, table);
	FILE* fp = fopen(m_currentFileName.c_str(), "w");
	if(!fp)
	{
		string msg = string("The session file ") + m_currentFileName + " could not be created!";
		Gtk::MessageDialog errdlg(msg, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
		errdlg.set_title("Cannot save session\n");
		errdlg.run();
		return;
	}
	if(config.length() != fwrite(config.c_str(), 1, config.length(), fp))
	{
		string msg = string("Error writing to session file ") + m_currentFileName + "!";
		Gtk::MessageDialog errdlg(msg, false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
		errdlg.set_title("Cannot save session\n");
		errdlg.run();
	}
	fclose(fp);

	//Serialize waveform data if needed
	if(saveWaveforms)
		SerializeWaveforms(table);
}

string OscilloscopeWindow::SerializeConfiguration(bool saveLayout, IDTable& table)
{
	string config = "";

	//TODO: save metadata

	//Save instrument config regardless, since data etc needs it
	config += SerializeInstrumentConfiguration(table);

	//Decodes depend on scope channels, but need to happen before UI elements that use them
	if(!Filter::GetAllInstances().empty())
		config += SerializeFilterConfiguration(table);

	//UI config
	if(saveLayout)
		config += SerializeUIConfiguration(table);

	return config;
}

/**
	@brief Serialize the configuration for all oscilloscopes
 */
string OscilloscopeWindow::SerializeInstrumentConfiguration(IDTable& table)
{
	string config = "instruments:\n";

	for(auto scope : m_scopes)
		config += scope->SerializeConfiguration(table);

	return config;
}

/**
	@brief Serialize the configuration for all protocol decoders
 */
string OscilloscopeWindow::SerializeFilterConfiguration(IDTable& table)
{
	string config = "decodes:\n";

	auto set = Filter::GetAllInstances();
	for(auto d : set)
		config += d->SerializeConfiguration(table);

	return config;
}

string OscilloscopeWindow::SerializeUIConfiguration(IDTable& table)
{
	char tmp[1024];
	string config = "ui_config:\n";

	config += "    window:\n";
	snprintf(tmp, sizeof(tmp), "        width: %d\n", get_width());
	config += tmp;
	snprintf(tmp, sizeof(tmp), "        height: %d\n", get_height());
	config += tmp;

	//Waveform areas
	config += "    areas:\n";
	for(auto area : m_waveformAreas)
		table.emplace(area);
	for(auto area : m_waveformAreas)
	{
		int id = table[area];
		snprintf(tmp, sizeof(tmp), "        area%d:\n", id);
		config += tmp;
		snprintf(tmp, sizeof(tmp), "            id:          %d\n", id);
		config += tmp;
		snprintf(tmp, sizeof(tmp), "            persistence: %d\n", area->GetPersistenceEnabled());
		config += tmp;

		//Channels
		//By the time we get here, all channels should be accounted for.
		//So there should be no reason to assign names to channels at this point - just use what's already there
		auto chan = area->GetChannel();
		snprintf(tmp, sizeof(tmp), "            channel:     %d\n", table[chan.m_channel]);
		config += tmp;
		snprintf(tmp, sizeof(tmp), "            stream:      %zu\n", chan.m_stream);
		config += tmp;

		//Overlays
		if(area->GetOverlayCount() != 0)
		{
			snprintf(tmp, sizeof(tmp), "            overlays:\n");
			config += tmp;

			for(size_t i=0; i<area->GetOverlayCount(); i++)
			{
				int oid = table[area->GetOverlay(i).m_channel];

				snprintf(tmp, sizeof(tmp), "                overlay%d:\n", oid);
				config += tmp;
				snprintf(tmp, sizeof(tmp), "                    id:      %d\n", oid);
				config += tmp;
				snprintf(tmp, sizeof(tmp), "                    stream:  %zu\n", area->GetOverlay(i).m_stream);
				config += tmp;
			}
		}
	}

	//Waveform groups
	config += "    groups: \n";
	for(auto group : m_waveformGroups)
		table.emplace(&group->m_frame);
	for(auto group : m_waveformGroups)
		config += group->SerializeConfiguration(table);

	//Splitters
	config += "    splitters: \n";
	for(auto split : m_splitters)
		table.emplace(split);
	for(auto split : m_splitters)
	{
		//Splitter config
		int sid = table[split];
		snprintf(tmp, sizeof(tmp), "        split%d: \n", sid);
		config += tmp;
		snprintf(tmp, sizeof(tmp), "            id:     %d\n", sid);
		config += tmp;

		if(split->get_orientation() == Gtk::ORIENTATION_HORIZONTAL)
			config +=  "            dir:    h\n";
		else
			config +=  "            dir:    v\n";

		//Splitter position
		snprintf(tmp, sizeof(tmp), "            split:  %d\n", split->get_position());
		config += tmp;

		//Children
		snprintf(tmp, sizeof(tmp), "            child0: %d\n", table[split->get_child1()]);
		config += tmp;
		snprintf(tmp, sizeof(tmp), "            child1: %d\n", table[split->get_child2()]);
		config += tmp;
	}

	//Top level splitter
	for(auto split : m_splitters)
	{
		if(split->get_parent() == &m_vbox)
		{
			snprintf(tmp, sizeof(tmp), "    top: %d\n", table[split]);
			config += tmp;
		}
	}

	return config;
}

/**
	@brief Serialize all waveforms for the session
 */
void OscilloscopeWindow::SerializeWaveforms(IDTable& table)
{
	lock_guard<recursive_mutex> lock(m_waveformDataMutex);

	char cwd[PATH_MAX];
	getcwd(cwd, PATH_MAX);
	chdir(m_currentDataDirName.c_str());

	const auto directories = ::Glob("scope_*", true);

	for(const auto& directory: directories)
		::RemoveDirectory(directory);

	chdir(cwd);

	//Create and show progress dialog
	FileProgressDialog progress;
	progress.show();
	float progress_per_scope = 1.0f / m_scopes.size();

	//Serialize waveforms for each of our instruments
	for(size_t i=0; i<m_scopes.size(); i++)
	{
		m_historyWindows[m_scopes[i]]->SerializeWaveforms(
			m_currentDataDirName,
			table,
			progress,
			i*progress_per_scope,
			progress_per_scope);
	}
}

void OscilloscopeWindow::OnAlphaChanged()
{
	ClearAllPersistence();
}

void OscilloscopeWindow::OnTriggerProperties(Oscilloscope* scope)
{
	//TODO: make this dialog modeless
	TriggerPropertiesDialog dlg(this, scope);
	if(Gtk::RESPONSE_OK != dlg.run())
		return;
	dlg.ConfigureTrigger();

	//Redraw the timeline and all waveform areas in case we changed the trigger channel etc
	for(auto g : m_waveformGroups)
		g->m_timeline.queue_draw();
	for(auto a : m_waveformAreas)
		a->queue_draw();
}

void OscilloscopeWindow::OnEyeColorChanged(string color, Gtk::RadioMenuItem* item)
{
	if(!item->get_active())
		return;

	m_eyeColor = color;
	for(auto v : m_waveformAreas)
		v->queue_draw();
}

/**
	@brief Returns a list of named color ramps
 */
vector<string> OscilloscopeWindow::GetEyeColorNames()
{
	vector<string> ret;
	for(auto it : m_eyeFiles)
		ret.push_back(it.first);
	sort(ret.begin(), ret.end());
	return ret;
}

void OscilloscopeWindow::OnHistory()
{
	if(m_btnHistory.get_active())
	{
		for(auto it : m_historyWindows)
		{
			it.second->show();
			it.second->grab_focus();
		}
	}
	else
	{
		for(auto it : m_historyWindows)
			it.second->hide();
	}
}

/**
	@brief Moves a waveform to the "best" group.

	Current heuristics:
		Eye pattern:
			Always make a new group below the current one
		Otherwise:
			Move to the first group with the same X axis unit.
			If none found, move below current
 */
void OscilloscopeWindow::MoveToBestGroup(WaveformArea* w)
{
	auto stream = w->GetChannel();
	auto eye = dynamic_cast<EyePattern*>(stream.m_channel);

	if(!eye)
	{
		for(auto g : m_waveformGroups)
		{
			g->m_timeline.RefreshUnits();
			if(stream.GetXAxisUnits() == g->m_timeline.GetXAxisUnits())
			{
				OnMoveToExistingGroup(w, g);
				return;
			}
		}
	}

	OnMoveNewBelow(w);
}

void OscilloscopeWindow::OnMoveNewRight(WaveformArea* w)
{
	OnMoveNew(w, true);
}

void OscilloscopeWindow::OnMoveNewBelow(WaveformArea* w)
{
	OnMoveNew(w, false);
}

void OscilloscopeWindow::SplitGroup(Gtk::Widget* frame, WaveformGroup* group, bool horizontal)
{
	//Hierarchy is WaveformArea -> WaveformGroup waveform box -> WaveformGroup box ->
	//WaveformGroup frame -> WaveformGroup event box -> splitter
	auto split = dynamic_cast<Gtk::Paned*>(frame->get_parent());
	if(split == NULL)
	{
		LogError("parent isn't a splitter\n");
		return;
	}

	//See what the widget's current parenting situation is.
	//We might have a free splitter area free already!
	Gtk::Paned* csplit = NULL;
	if(horizontal)
		csplit = dynamic_cast<Gtk::HPaned*>(split);
	else
		csplit = dynamic_cast<Gtk::VPaned*>(split);
	if( (csplit != NULL) && (split->get_child2() == NULL) )
	{
		split->pack2(group->m_frame);
		split->show_all();
	}

	//Split the current parent
	else
	{
		//Create a new splitter
		Gtk::Paned* nsplit;
		if(horizontal)
			nsplit = new Gtk::HPaned;
		else
			nsplit = new Gtk::VPaned;
		m_splitters.emplace(nsplit);

		//Take the current frame out of the parent group so we have room for the splitter
		if(frame == split->get_child1())
		{
			split->remove(*frame);
			split->pack1(*nsplit);
		}
		else
		{
			split->remove(*frame);
			split->pack2(*nsplit);
		}

		nsplit->pack1(*frame);
		nsplit->pack2(group->m_frame);
		split->show_all();
	}
}

void OscilloscopeWindow::OnMoveNew(WaveformArea* w, bool horizontal)
{
	//Make a new group
	auto group = new WaveformGroup(this);
	group->m_pixelsPerXUnit = w->m_group->m_pixelsPerXUnit;
	m_waveformGroups.emplace(group);

	//Split the existing group and add the new group to it
	SplitGroup(w->GetGroupFrame(), group, horizontal);

	//Move the waveform into the new group
	OnMoveToExistingGroup(w, group);
}

void OscilloscopeWindow::OnCopyNew(WaveformArea* w, bool horizontal)
{
	//Make a new group
	auto group = new WaveformGroup(this);
	group->m_pixelsPerXUnit = w->m_group->m_pixelsPerXUnit;
	m_waveformGroups.emplace(group);

	//Split the existing group and add the new group to it
	SplitGroup(w->GetGroupFrame(), group, horizontal);

	//Make a copy of the current waveform view and add to that group
	OnCopyToExistingGroup(w, group);
}

void OscilloscopeWindow::OnMoveToExistingGroup(WaveformArea* w, WaveformGroup* ngroup)
{
	auto oldgroup = w->m_group;

	w->m_group = ngroup;
	w->get_parent()->remove(*w);

	if(w->GetChannel().m_channel->GetType() == OscilloscopeChannel::CHANNEL_TYPE_DIGITAL)
		ngroup->m_waveformBox.pack_start(*w, Gtk::PACK_SHRINK);
	else
		ngroup->m_waveformBox.pack_start(*w);

	//Move stats related to this trace to the new group
	set<StreamDescriptor> chans;
	chans.emplace(w->GetChannel());
	for(size_t i=0; i<w->GetOverlayCount(); i++)
		chans.emplace(w->GetOverlay(i));
	for(auto chan : chans)
	{
		if(oldgroup->IsShowingStats(chan))
		{
			oldgroup->DisableStats(chan);
			ngroup->EnableStats(chan);
		}
	}

	//Remove any groups that no longer have any waveform views in them,
	//or splitters that only have one child
	GarbageCollectGroups();
}

void OscilloscopeWindow::OnCopyNewRight(WaveformArea* w)
{
	OnCopyNew(w, true);
}

void OscilloscopeWindow::OnCopyNewBelow(WaveformArea* w)
{
	OnCopyNew(w, false);
}

void OscilloscopeWindow::OnCopyToExistingGroup(WaveformArea* w, WaveformGroup* ngroup)
{
	//Create a new waveform area that looks like the existing one (not an exact copy)
	WaveformArea* nw = new WaveformArea(w);
	m_waveformAreas.emplace(nw);

	//Then add it like normal
	nw->m_group = ngroup;
	if(nw->GetChannel().m_channel->GetType() == OscilloscopeChannel::CHANNEL_TYPE_DIGITAL)
		ngroup->m_waveformBox.pack_start(*nw, Gtk::PACK_SHRINK);
	else
		ngroup->m_waveformBox.pack_start(*nw);
	nw->show();

	//Add stats if needed
	set<StreamDescriptor> chans;
	chans.emplace(w->GetChannel());
	for(size_t i=0; i<w->GetOverlayCount(); i++)
		chans.emplace(w->GetOverlay(i));
	for(auto chan : chans)
	{
		if(w->m_group->IsShowingStats(chan))
			ngroup->EnableStats(chan);
	}
}

void OscilloscopeWindow::GarbageCollectGroups()
{
	//Remove groups with no waveforms (any attached measurements will be deleted)
	std::set<WaveformGroup*> groupsToRemove;
	for(auto g : m_waveformGroups)
	{
		if(g->m_waveformBox.get_children().empty())
			groupsToRemove.emplace(g);
	}
	for(auto g : groupsToRemove)
	{
		auto parent = g->m_frame.get_parent();
		parent->remove(g->m_frame);
		delete g;
		m_waveformGroups.erase(g);
	}

	//If a splitter only has a group in the second half, move it to the first
	for(auto s : m_splitters)
	{
		auto first = s->get_child1();
		auto second = s->get_child2();
		if( (first == NULL) && (second != NULL) )
		{
			s->remove(*second);
			s->pack1(*second);
		}
	}

	//If a splitter only has a group in the first half, move it to the parent splitter and delete it
	//(if there is one)
	set<Gtk::Paned*> splittersToRemove;
	for(auto s : m_splitters)
	{
		auto first = s->get_child1();
		auto second = s->get_child2();
		if( (first != NULL) && (second == NULL) )
		{
			//Child of another splitter, move us to it
			auto parent = s->get_parent();
			if(parent != &m_vbox)
			{
				//Move our child to the empty half of our parent
				auto pparent = dynamic_cast<Gtk::Paned*>(parent);
				if(pparent->get_child1() == s)
				{
					s->remove(*first);
					pparent->remove(*s);
					pparent->pack1(*first);
				}
				else
				{
					s->remove(*first);
					pparent->remove(*s);
					pparent->pack2(*first);
				}

				//Delete us
				splittersToRemove.emplace(s);
			}

			//If this is the top level splitter, we have no higher level to move it to
			//so no action required? or do we delete the splitter entirely and only have us in the vbox?
		}
	}

	for(auto s : splittersToRemove)
	{
		m_splitters.erase(s);
		delete s;
	}

	//Hide stat display if there's no stats in the group
	for(auto g : m_waveformGroups)
	{
		if(g->m_columnToIndexMap.empty())
			g->m_measurementView.hide();
		else
			g->m_measurementView.show_all();
	}
}

void OscilloscopeWindow::OnFullscreen()
{
	m_fullscreen = !m_fullscreen;

	//Enter fullscreen mode
	if(m_fullscreen)
	{
		//Update toolbar button icon
		m_btnFullscreen.set_icon_widget(m_iconExitFullscreen);
		m_iconExitFullscreen.show();

		int x;
		int y;
		get_position(x, y);
		m_originalRect = Gdk::Rectangle(x, y, get_width(), get_height());

		//Figure out the size we need to be in order to become fullscreen
		auto screen = get_screen();
		int mon = screen->get_monitor_at_window(get_window());
		Gdk::Rectangle rect;
		screen->get_monitor_geometry(mon, rect);

		//Make us fake-fullscreen (on top of everything else and occupying the entire monitor).
		//We can't just use Gtk::Window::fullscreen() because this messes with popup dialogs
		//like protocol analyzers.
		set_keep_above();
		set_decorated(false);
		move(rect.get_x(), rect.get_y());
		resize(rect.get_width(), rect.get_height());
	}

	//Revert to our old setup
	else
	{
		set_keep_above(false);
		set_decorated();
		resize(m_originalRect.get_width(), m_originalRect.get_height());
		move(m_originalRect.get_x(), m_originalRect.get_y());

		//Update toolbar button icon
		m_btnFullscreen.set_icon_widget(m_iconEnterFullscreen);
	}
}

void OscilloscopeWindow::OnClearSweeps()
{
	lock_guard<recursive_mutex> lock(m_waveformDataMutex);

	//TODO: clear regular waveform data and history too?

	//Clear integrated data from all pfilters
	auto filters = Filter::GetAllInstances();
	for(auto f : filters)
		f->ClearSweeps();

	//Clear persistence on all groups
	for(auto g : m_waveformGroups)
	{
		g->ClearStatistics();
		ClearPersistence(g);
	}
}

void OscilloscopeWindow::OnRefreshConfig()
{
	for(auto scope : m_scopes)
		scope->FlushConfigCache();
}

void OscilloscopeWindow::OnAutofitHorizontal(WaveformGroup* group)
{
	auto areas = GetAreasInGroup(group);

	//Figure out how wide the widest waveform in the group is, in pixels
	float width = 0;
	for(auto a : areas)
		width = max(width, a->GetPlotWidthPixels());

	//Find all waveforms visible in any area within the group
	set<WaveformBase*> wfms;
	for(auto a : areas)
	{
		auto data = a->GetChannel().GetData();
		if(data != NULL)
			wfms.emplace(data);
		for(size_t i=0; i < a->GetOverlayCount(); i++)
		{
			auto o = a->GetOverlay(i);
			data = o.GetData();
			if(data != NULL)
				wfms.emplace(data);
		}
	}

	//Find how long the longest waveform is.
	//Horizontal displacement doesn't matter for now, only total length.
	int64_t duration = 0;
	for(auto w : wfms)
	{
		size_t len = w->m_offsets.size();
		if(len < 2)
			continue;
		size_t end = len - 1;

		int64_t delta = w->m_offsets[end] + w->m_durations[end] - w->m_offsets[0];
		duration = max(duration, delta * w->m_timescale);
	}

	//Change the zoom
	group->m_pixelsPerXUnit = width / duration;
	group->m_xAxisOffset = 0;

	ClearPersistence(group, false, true);
}

/**
	@brief Zoom in, keeping timestamp "target" at the same position within the group
 */
void OscilloscopeWindow::OnZoomInHorizontal(WaveformGroup* group, int64_t target)
{
	//Calculate the *current* position of the target within the window
	float delta = target - group->m_xAxisOffset;

	//Change the zoom
	float step = 1.5;
	group->m_pixelsPerXUnit *= step;
	group->m_xAxisOffset = target - (delta/step);

	ClearPersistence(group, false, true);
}

/**
	@brief Zoom out, keeping timestamp "target" at the same position within the group
 */
void OscilloscopeWindow::OnZoomOutHorizontal(WaveformGroup* group, int64_t target)
{
	//Figure out how wide the widest waveform in the group is, in X axis units
	float width = 0;
	auto areas = GetAreasInGroup(group);
	for(auto a : areas)
		width = max(width, a->GetPlotWidthXUnits());

	//Find all waveforms visible in any area within the group
	set<WaveformBase*> wfms;
	for(auto a : areas)
	{
		auto data = a->GetChannel().GetData();
		if(data != NULL)
			wfms.emplace(data);
		for(size_t i=0; i < a->GetOverlayCount(); i++)
		{
			auto o = a->GetOverlay(i);
			data = o.GetData();
			if(data != NULL)
				wfms.emplace(data);
		}
	}

	//Find how long the longest waveform is.
	//Horizontal displacement doesn't matter for now, only total length.
	int64_t duration = 0;
	for(auto w : wfms)
	{
		//Spectrograms need special treatment
		auto spec = dynamic_cast<SpectrogramWaveform*>(w);
		if(spec)
			duration = max(duration, spec->GetDuration());

		else
		{
			size_t len = w->m_offsets.size();
			if(len < 2)
				continue;
			size_t end = len - 1;

			int64_t delta = w->m_offsets[end] + w->m_durations[end] - w->m_offsets[0];
			duration = max(duration, delta * w->m_timescale);
		}
	}

	//If the view is already wider than the longest waveform, don't allow further zooming
	if(width > duration)
		return;

	//Calculate the *current* position of the target within the window
	float delta = target - group->m_xAxisOffset;

	//Change the zoom
	float step = 1.5;
	group->m_pixelsPerXUnit /= step;
	group->m_xAxisOffset = target - (delta*step);

	ClearPersistence(group, false, true);
}

vector<WaveformArea*> OscilloscopeWindow::GetAreasInGroup(WaveformGroup* group)
{
	auto children = group->m_vbox.get_children();


	vector<WaveformArea*> areas;
	for(auto w : children)
	{
		//Redraw all views in the waveform box
		auto box = dynamic_cast<Gtk::Box*>(w);
		if(box)
		{
			auto bchildren = box->get_children();
			for(auto a : bchildren)
			{
				auto area = dynamic_cast<WaveformArea*>(a);
				if(area != NULL && w->get_realized())
					areas.push_back(area);
			}
		}
	}
	return areas;
}

void OscilloscopeWindow::ClearPersistence(WaveformGroup* group, bool geometry_dirty, bool position_dirty)
{
	auto areas = GetAreasInGroup(group);

	//Mark each area as dirty and map the buffers needed for update
	for(auto w : areas)
	{
		w->CalculateOverlayPositions();
		w->ClearPersistence(false);

		if(geometry_dirty)
			w->MapAllBuffers(true);
		else if(position_dirty)
			w->MapAllBuffers(false);
	}

	//Do the actual updates
	float alpha = GetTraceAlpha();
	if(geometry_dirty || position_dirty)
	{
		lock_guard<recursive_mutex> lock(m_waveformDataMutex);

		//Make the list of data to update
		vector<WaveformRenderData*> data;
		float coeff = -1;
		for(auto w : areas)
		{
			if(coeff < 0)
				coeff = w->GetPersistenceDecayCoefficient();

			w->GetAllRenderData(data);
		}

		//Do the updates in parallel
		#pragma omp parallel for
		for(size_t i=0; i<data.size(); i++)
			WaveformArea::PrepareGeometry(data[i], geometry_dirty, alpha, coeff);

		//Clean up
		for(auto w : areas)
		{
			w->SetNotDirty();
			w->UnmapAllBuffers(geometry_dirty);
		}
	}

	//Submit update requests for each area (and the timeline)
	auto children = group->m_vbox.get_children();
	for(auto w : children)
		w->queue_draw();
}

void OscilloscopeWindow::ClearAllPersistence()
{
	for(auto g : m_waveformGroups)
		ClearPersistence(g, true, false);
}

void OscilloscopeWindow::OnQuit()
{
	close();
}

void OscilloscopeWindow::OnAddChannel(StreamDescriptor chan)
{
	//If we have no splitters, make one
	if(m_splitters.empty())
	{
		auto split = new Gtk::VPaned;
		m_vbox.pack_start(*split);
		m_splitters.emplace(split);
	}

	//If all waveform groups were closed, recreate one
	if(m_waveformGroups.empty())
	{
		auto split = *m_splitters.begin();
		auto group = new WaveformGroup(this);
		m_waveformGroups.emplace(group);
		split->pack1(group->m_frame);
		split->show_all();
		group->m_measurementView.hide();
	}

	auto w = DoAddChannel(chan, *m_waveformGroups.begin());
	MoveToBestGroup(w);

	RefreshTimebasePropertiesDialog();
}

void OscilloscopeWindow::RefreshTimebasePropertiesDialog()
{
	if(m_timebasePropertiesDialog)
	{
		if(m_timebasePropertiesDialog->is_visible())
			m_timebasePropertiesDialog->RefreshAll();

		else
		{
			delete m_timebasePropertiesDialog;
			m_timebasePropertiesDialog = nullptr;
		}
	}
}

WaveformArea* OscilloscopeWindow::DoAddChannel(StreamDescriptor chan, WaveformGroup* ngroup, WaveformArea* ref)
{
	//Create the viewer
	auto w = new WaveformArea(chan, this);
	w->m_group = ngroup;
	m_waveformAreas.emplace(w);

	if(chan.m_channel->GetType() == OscilloscopeChannel::CHANNEL_TYPE_DIGITAL)
		ngroup->m_waveformBox.pack_start(*w, Gtk::PACK_SHRINK);
	else
		ngroup->m_waveformBox.pack_start(*w);

	//Move the new trace after the reference trace, if one was provided
	if(ref != NULL)
	{
		auto children = ngroup->m_waveformBox.get_children();
		for(size_t i=0; i<children.size(); i++)
		{
			if(children[i] == ref)
				ngroup->m_waveformBox.reorder_child(*w, i+1);
		}
	}

	//Refresh the channels menu since the newly added channel might create new banking conflicts
	RefreshChannelsMenu();

	w->show();
	return w;
}

void OscilloscopeWindow::OnRemoveChannel(WaveformArea* w)
{
	//Get rid of the channel
	w->get_parent()->remove(*w);
	m_waveformAreas.erase(w);
	delete w;

	//Clean up in case it was the last channel in the group
	GarbageCollectGroups();
	RefreshFilterGraphEditor();

	RefreshTimebasePropertiesDialog();
}

void OscilloscopeWindow::GarbageCollectAnalyzers()
{
	//Check out our analyzers and see if any of them now have no references other than the analyzer window itself.
	//If the analyzer is hidden, and there's no waveform views for it, get rid of it
	set<ProtocolAnalyzerWindow*> garbage;
	for(auto a : m_analyzers)
	{
		//It's visible. Still active.
		if(a->get_visible())
			continue;

		//If there is only one reference, it's to the analyzer itself.
		//Which is hidden, so we want to get rid of it.
		auto chan = a->GetDecoder();
		if(chan->GetRefCount() == 1)
			garbage.emplace(a);
	}

	for(auto a : garbage)
	{
		m_analyzers.erase(a);
		delete a;
	}

	//Need to reload the menu in case we deleted the last reference to something
	RefreshChannelsMenu();
	RefreshAnalyzerMenu();
}

/**
	@brief Returns true if we have at least one scope that isn't offline
 */
bool OscilloscopeWindow::HasOnlineScopes()
{
	for(auto scope : m_scopes)
	{
		if(!scope->IsOffline())
			return true;
	}
	return false;
}

/**
	@brief See if we have waveforms ready to process
 */
bool OscilloscopeWindow::CheckForPendingWaveforms()
{
	//No online scopes to poll? Re-run the filter graph
	if(!HasOnlineScopes())
		return m_triggerArmed;

	//Wait for every online scope to have triggered
	for(auto scope : m_scopes)
	{
		if(scope->IsOffline())
			continue;
		if(!scope->HasPendingWaveforms())
			return false;
	}

	//Keep track of when the primary instrument triggers.
	if(m_multiScopeFreeRun)
	{
		//See when the primary triggered
		if( (m_tPrimaryTrigger < 0) && m_scopes[0]->HasPendingWaveforms() )
			m_tPrimaryTrigger = GetTime();

		//All instruments should trigger within 1 sec (arbitrary threshold) of the primary.
		//If it's been longer than that, something went wrong. Discard all pending data and re-arm the trigger.
		double twait = GetTime() - m_tPrimaryTrigger;
		if( (m_tPrimaryTrigger > 0) && ( twait > 1 ) )
		{
			LogWarning("Timed out waiting for one or more secondary instruments to trigger (%.2f ms). Resetting...\n",
				twait*1000);

			//Cancel any pending triggers
			OnStop();

			//Discard all pending waveform data
			for(auto scope : m_scopes)
			{
				//Don't touch anything offline
				if(scope->IsOffline())
					continue;

				scope->IDPing();
				scope->ClearPendingWaveforms();
			}

			//Re-arm the trigger and get back to polling
			OnStart();
			return false;
		}
	}

	//If we get here, we had waveforms on all instruments
	return true;
}

/**
	@brief Pull the waveform data out of the queue and make it current
 */
void OscilloscopeWindow::DownloadWaveforms()
{
	lock_guard<recursive_mutex> lock(m_waveformDataMutex);

	//Process the waveform data from each instrument
	for(auto scope : m_scopes)
	{
		//Don't touch anything offline
		if(scope->IsOffline())
			continue;

		//Make sure we don't free the old waveform data
		for(size_t i=0; i<scope->GetChannelCount(); i++)
		{
			auto chan = scope->GetChannel(i);
			for(size_t j=0; j<chan->GetStreamCount(); j++)
				chan->Detach(j);
		}

		//Download the data
		scope->PopPendingWaveform();
	}

	//If we're in offline one-shot mode, disarm the trigger
	if( (m_scopes.empty()) && m_triggerOneShot)
		m_triggerArmed = false;
}

/**
	@brief Handles updating things after all instruments have downloaded their new waveforms
 */
void OscilloscopeWindow::OnAllWaveformsUpdated(bool reconfiguring, bool updateFilters)
{
	lock_guard<recursive_mutex> lock(m_waveformDataMutex);

	m_totalWaveforms ++;

	//Update the status
	UpdateStatusBar();
	if(updateFilters)
		RefreshAllFilters();

	//Update protocol analyzers
	//TODO: ideal would be to delete all old packets from analyzers then update them with current ones.
	//This would allow changing settings on a protocol to update correctly.
	if(!reconfiguring)
	{
		for(auto a : m_analyzers)
			a->OnWaveformDataReady();
	}

	//Update waveform areas.
	//Skip this if loading a file from the command line and loading isn't done
	if(WaveformArea::IsGLInitComplete())
	{
		//Map all of the buffers we need to update in each area
		for(auto w : m_waveformAreas)
		{
			w->OnWaveformDataReady();
			w->CalculateOverlayPositions();
			w->MapAllBuffers(true);
		}

		float alpha = GetTraceAlpha();

		//Make the list of data to update (waveforms plus overlays)
		vector<WaveformRenderData*> data;
		float coeff = -1;
		for(auto w : m_waveformAreas)
		{
			w->GetAllRenderData(data);

			if(coeff < 0)
				coeff = w->GetPersistenceDecayCoefficient();
		}

		//Do the updates in parallel
		#pragma omp parallel for
		for(size_t i=0; i<data.size(); i++)
			WaveformArea::PrepareGeometry(data[i], true, alpha, coeff);

		//Clean up
		for(auto w : m_waveformAreas)
		{
			w->SetNotDirty();
			w->UnmapAllBuffers(true);
		}

		//Submit update requests for each area
		for(auto w : m_waveformAreas)
			w->queue_draw();
	}

	if(!reconfiguring)
	{
		//Redraw timeline in case trigger config was updated during the waveform download
		for(auto g : m_waveformGroups)
			g->m_timeline.queue_draw();

		//Update the trigger sync wizard, if it's active
		if(m_scopeSyncWizard && m_scopeSyncWizard->is_visible())
			m_scopeSyncWizard->OnWaveformDataReady();

		//Check if a conditional halt applies
		int64_t timestamp;
		if(m_haltConditionsDialog.ShouldHalt(timestamp))
		{
			auto chan = m_haltConditionsDialog.GetHaltChannel();

			OnStop();

			if(m_haltConditionsDialog.ShouldMoveToHalt())
			{
				//Find the waveform area(s) for this channel
				for(auto a : m_waveformAreas)
				{
					if(a->GetChannel() == chan)
					{
						a->m_group->m_xAxisOffset = timestamp;
						a->m_group->m_frame.queue_draw();
					}

					for(size_t i=0; i<a->GetOverlayCount(); i++)
					{
						if(a->GetOverlay(i) == chan)
						{
							a->m_group->m_xAxisOffset = timestamp;
							a->m_group->m_frame.queue_draw();
						}
					}
				}
			}
		}
	}
}

void OscilloscopeWindow::RefreshAllFilters()
{
	lock_guard<recursive_mutex> lock(m_waveformDataMutex);

	SyncFilterColors();

	Filter::ClearAnalysisCache();

	set<Filter*> filters;
	{
		lock_guard<mutex> lock2(m_filterUpdatingMutex);
		filters = Filter::GetAllInstances();
	}
	for(auto f : filters)
		f->SetDirty();

	//Prepare to topologically sort filter nodes into blocks capable of parallel evaluation.
	//Block 0 may only depend on physical scope channels.
	//Block 1 may depend on decodes in block 0 or physical channels.
	//Block 2 may depend on 1/0/physical, etc.
	typedef vector<Filter*> FilterBlock;
	vector<FilterBlock> blocks;

	//Working set starts out as all decoders
	auto working = filters;

	//Each iteration, put all decodes that only depend on previous blocks into this block.
	for(int block=0; !working.empty(); block++)
	{
		FilterBlock current_block;
		for(auto w : working)
		{
			auto d = static_cast<Filter*>(w);

			//Check if we have any inputs that are still in the working set.
			bool ok = true;
			for(size_t i=0; i<d->GetInputCount(); i++)
			{
				auto in = d->GetInput(i).m_channel;
				if(working.find((Filter*)in) != working.end())
				{
					ok = false;
					break;
				}
			}

			//All inputs are in previous blocks, we're good to go for the current block
			if(ok)
				current_block.push_back(d);
		}

		//Anything we assigned this iteration shouldn't be in the working set for next time.
		//It does, however, have to get saved in the output block.
		for(auto d : current_block)
			working.erase(d);
		blocks.push_back(current_block);
	}

	//Evaluate the blocks, taking advantage of parallelism between them
	for(auto& block : blocks)
	{
		#pragma omp parallel for
		for(size_t i=0; i<block.size(); i++)
			block[i]->RefreshIfDirty();
	}

	//Update statistic displays after the filter graph update is complete
	for(auto g : m_waveformGroups)
		g->RefreshMeasurements();
}

void OscilloscopeWindow::RefreshAllViews()
{
	for(auto a : m_waveformAreas)
		a->queue_draw();
}

void OscilloscopeWindow::UpdateStatusBar()
{
	char tmp[256];
	if(m_scopes.empty())
		return;

	//TODO: redo this for multiple scopes
	auto scope = m_scopes[0];
	auto trig = scope->GetTrigger();
	if(trig)
	{
		auto chan = trig->GetInput(0).m_channel;
		if(chan == NULL)
		{
			LogWarning("Trigger channel is NULL\n");
			return;
		}
		string name = chan->GetHwname();
		Unit volts(Unit::UNIT_VOLTS);
		m_triggerConfigLabel.set_label(volts.PrettyPrint(trig->GetLevel()));
	}

	//Update WFM/s counter
	if(m_lastWaveformTimes.size() >= 2)
	{
		double first = m_lastWaveformTimes[0];
		double last = m_lastWaveformTimes[m_lastWaveformTimes.size() - 1];
		double dt = last - first;
		double wps = m_lastWaveformTimes.size() / dt;
		snprintf(tmp, sizeof(tmp), "%zu WFMs, %.2f WFM/s", m_totalWaveforms, wps);
		m_waveformRateLabel.set_label(tmp);
	}
}

void OscilloscopeWindow::OnStart()
{
	ArmTrigger(TRIGGER_TYPE_NORMAL);
}

void OscilloscopeWindow::OnStartSingle()
{
	ArmTrigger(TRIGGER_TYPE_SINGLE);
}

void OscilloscopeWindow::OnForceTrigger()
{
	ArmTrigger(TRIGGER_TYPE_FORCED);
}

void OscilloscopeWindow::OnStop()
{
	m_multiScopeFreeRun = false;
	m_triggerArmed = false;

	for(auto scope : m_scopes)
	{
		scope->Stop();

		//Clear out any pending data (the user doesn't want it, and we don't want stale stuff hanging around)
		scope->ClearPendingWaveforms();
	}
}

void OscilloscopeWindow::ArmTrigger(TriggerType type)
{
	bool oneshot = (type == TRIGGER_TYPE_FORCED) || (type == TRIGGER_TYPE_SINGLE);
	m_triggerOneShot = oneshot;

	if(!HasOnlineScopes())
	{
		m_tArm = GetTime();
		m_triggerArmed = true;
		return;
	}

	/*
		If we have multiple scopes, always use single trigger to keep them synced.
		Multi-trigger can lead to race conditions and dropped triggers if we're still downloading a secondary
		instrument's waveform and the primary re-arms.

		Also, order of arming is critical. Secondaries must be completely armed before the primary (instrument 0) to
		ensure that the primary doesn't trigger until the secondaries are ready for the event.
	*/
	m_tPrimaryTrigger = -1;
	if(!oneshot && (m_scopes.size() > 1) )
		m_multiScopeFreeRun = true;
	else
		m_multiScopeFreeRun = false;

	//In multi-scope mode, make sure all scopes are stopped with no pending waveforms
	if(m_scopes.size() > 1)
	{
		for(ssize_t i=m_scopes.size()-1; i >= 0; i--)
		{
			if(m_scopes[i]->PeekTriggerArmed())
				m_scopes[i]->Stop();

			if(m_scopes[i]->HasPendingWaveforms())
			{
				LogWarning("Scope %s had pending waveforms before arming\n", m_scopes[i]->m_nickname.c_str());
				m_scopes[i]->ClearPendingWaveforms();
			}
		}
	}

	for(ssize_t i=m_scopes.size()-1; i >= 0; i--)
	{
		//If we have >1 scope, all secondaries always use single trigger synced to the primary's trigger output
		if(i > 0)
			m_scopes[i]->StartSingleTrigger();

		else
		{
			switch(type)
			{
				//Normal trigger: all scopes lock-step for multi scope
				//for single scope, use normal trigger
				case TRIGGER_TYPE_NORMAL:
					if(m_scopes.size() > 1)
						m_scopes[i]->StartSingleTrigger();
					else
						m_scopes[i]->Start();
					break;

				case TRIGGER_TYPE_AUTO:
					LogError("ArmTrigger(TRIGGER_TYPE_AUTO) not implemented\n");
					break;

				case TRIGGER_TYPE_SINGLE:
					m_scopes[i]->StartSingleTrigger();
					break;

				case TRIGGER_TYPE_FORCED:
					m_scopes[i]->ForceTrigger();
					break;

				default:
					break;
			}
		}

		//If we have multiple scopes, ping the secondaries to make sure the arm command went through
		if(i != 0)
		{
			double start = GetTime();

			while(!m_scopes[i]->PeekTriggerArmed())
			{
				//After 3 sec of no activity, time out
				//(must be longer than the default 2 sec socket timeout)
				double now = GetTime();
				if( (now - start) > 3)
				{
					LogWarning("Timeout waiting for scope %s to arm\n",  m_scopes[i]->m_nickname.c_str());
					m_scopes[i]->Stop();
					m_scopes[i]->StartSingleTrigger();
					start = now;
				}
			}

			//Scope is armed. Clear any garbage in the pending queue
			m_scopes[i]->ClearPendingWaveforms();
		}
	}
	m_tArm = GetTime();
	m_triggerArmed = true;
}

/**
	@brief Called when the history view selects an old waveform
 */
void OscilloscopeWindow::OnHistoryUpdated(bool refreshAnalyzers)
{
	lock_guard<recursive_mutex> lock(m_waveformDataMutex);

	//Stop triggering if we select a saved waveform
	OnStop();

	RefreshAllFilters();

	//Update the views
	for(auto w : m_waveformAreas)
	{
		if(w->get_realized())
			w->OnWaveformDataReady();
	}
	ClearAllPersistence();

	if(refreshAnalyzers)
	{
		for(auto a : m_analyzers)
			a->OnWaveformDataReady();
	}
}

/**
	@brief Remove protocol analyzer history prior to a given timestamp
 */
void OscilloscopeWindow::RemoveProtocolHistoryBefore(TimePoint timestamp)
{
	for(auto a : m_analyzers)
		a->RemoveHistoryBefore(timestamp);
}

void OscilloscopeWindow::JumpToHistory(TimePoint timestamp)
{
	//TODO:  this might not work too well if triggers aren't perfectly synced!
	for(auto it : m_historyWindows)
		it.second->JumpToHistory(timestamp);
}

void OscilloscopeWindow::OnTimebaseSettings()
{
	if(!m_timebasePropertiesDialog)
		m_timebasePropertiesDialog = new TimebasePropertiesDialog(this, m_scopes);
	m_timebasePropertiesDialog->show();
}

/**
	@brief Shows the synchronization dialog for connecting multiple scopes.
 */
void OscilloscopeWindow::OnScopeSync()
{
	if(m_scopes.size() > 1)
	{
		//Stop triggering
		OnStop();

		//Prepare sync
		if(!m_scopeSyncWizard)
			m_scopeSyncWizard = new ScopeSyncWizard(this);

		m_scopeSyncWizard->show();
		m_syncComplete = false;
	}
}

void OscilloscopeWindow::OnSyncComplete()
{
	m_syncComplete = true;
}

/**
	@brief Propagate name changes from one channel to filters that use it as input
 */
void OscilloscopeWindow::OnChannelRenamed(OscilloscopeChannel* chan)
{
	//Check all filters to see if they use this as input
	auto filters = Filter::GetAllInstances();
	for(auto f : filters)
	{
		//If using a custom name, don't change that
		if(!f->IsUsingDefaultName())
			continue;

		for(size_t i=0; i<f->GetInputCount(); i++)
		{
			//We matched!
			if(f->GetInput(i).m_channel == chan)
			{
				f->SetDefaultName();
				OnChannelRenamed(f);
				break;
			}
		}
	}

	//Check if we have any groups that are showing stats for it
	for(auto g : m_waveformGroups)
	{
		if(g->IsShowingStats(chan))
			g->OnChannelRenamed(chan);
	}
}

/**
	@brief Shows the halt conditions dialog
 */
void OscilloscopeWindow::OnHaltConditions()
{
	m_haltConditionsDialog.show();
	m_haltConditionsDialog.RefreshChannels();
}

/**
	@brief Generate a new waveform using a filter
 */
void OscilloscopeWindow::OnGenerateFilter(string name)
{
	//need to modeless dialog
	string color = GetDefaultChannelColor(g_numDecodes);
	m_pendingGenerator = Filter::CreateFilter(name, color);

	if(m_addFilterDialog)
		delete m_addFilterDialog;
	m_addFilterDialog = new FilterDialog(this, m_pendingGenerator, NULL);
	m_addFilterDialog->show();
	m_addFilterDialog->signal_delete_event().connect(sigc::mem_fun(*this, &OscilloscopeWindow::OnGenerateDialogClosed));

	//Add initial streams
	g_numDecodes ++;
	for(size_t i=0; i<m_pendingGenerator->GetStreamCount(); i++)
		OnAddChannel(StreamDescriptor(m_pendingGenerator, i));
}

/**
	@brief Handles a filter that was updated in such a way that the stream count changed
 */
void OscilloscopeWindow::OnStreamCountChanged(Filter* filter)
{
	//Step 1: Remove any views for streams that no longer exist
	set<WaveformArea*> areasToRemove;
	for(auto w : m_waveformAreas)
	{
		auto c = w->GetChannel();
		if( (c.m_channel == filter) && (c.m_stream >= filter->GetStreamCount() ) )
			areasToRemove.emplace(w);
	}
	for(auto w : areasToRemove)
		OnRemoveChannel(w);

	//Step 2: Create views for streams that were newly created
	for(size_t i=0; i<filter->GetStreamCount(); i++)
	{
		StreamDescriptor stream(filter, i);

		//TODO: can we do this faster than O(n^2) with a hash table or something?
		//Probably a non-issue for now because number of waveform areas isn't going to be too massive given
		//limitations on available screen real estate
		bool found = false;
		for(auto w : m_waveformAreas)
		{
			if(w->GetChannel() == stream)
			{
				found = true;
				break;
			}
		}

		if(!found)
			OnAddChannel(stream);
	}
}

bool OscilloscopeWindow::OnGenerateDialogClosed(GdkEventAny* /*ignored*/)
{
	//Commit any remaining pending changes
	m_addFilterDialog->ConfigureDecoder();

	//Done with the dialog
	delete m_addFilterDialog;
	m_addFilterDialog = NULL;
	return false;
}

/**
	@brief Update the generate / import waveform menus
 */
void OscilloscopeWindow::RefreshGenerateAndImportMenu()
{
	//Remove old ones
	auto children = m_generateMenu.get_children();
	for(auto c : children)
		m_generateMenu.remove(*c);
	children = m_importMenu.get_children();
	for(auto c : children)
		m_importMenu.remove(*c);

	//Add all filters that have no inputs
	vector<string> names;
	Filter::EnumProtocols(names);
	for(auto p : names)
	{
		//Create a test filter
		auto d = Filter::CreateFilter(p, "");
		if(d->GetInputCount() == 0)
		{
			auto item = Gtk::manage(new Gtk::MenuItem(p, false));

			//Add to the generate menu if the filter name doesn't contain "import"
			if(p.find("Import") == string::npos)
				m_generateMenu.append(*item);

			//Otherwise, add to the import menu (and trim "import" off the filter name)
			else
			{
				item->set_label(p.substr(0, p.length() - strlen(" Import")));
				m_importMenu.append(*item);
			}

			item->signal_activate().connect(
				sigc::bind<string>(sigc::mem_fun(*this, &OscilloscopeWindow::OnGenerateFilter), p));
		}
		delete d;
	}
}

/**
	@brief Update the channels menu when we connect to a new instrument
 */
void OscilloscopeWindow::RefreshChannelsMenu()
{
	//Remove the old items
	auto children = m_channelsMenu.get_children();
	for(auto c : children)
		m_channelsMenu.remove(*c);

	vector<OscilloscopeChannel*> chans;

	//Add new ones
	for(auto scope : m_scopes)
	{
		for(size_t i=0; i<scope->GetChannelCount(); i++)
		{
			auto chan = scope->GetChannel(i);

			//Skip channels that can't be enabled for some reason
			if(!scope->CanEnableChannel(i))
				continue;

			//Add a menu item - but not for the external trigger(s)
			if(chan->GetType() != OscilloscopeChannel::CHANNEL_TYPE_TRIGGER)
				chans.push_back(chan);
		}
	}

	//Add filters
	auto filters = Filter::GetAllInstances();
	for(auto f : filters)
		chans.push_back(f);

	//Create a menu item for each stream of each channel
	for(auto chan : chans)
	{
		auto nstreams = chan->GetStreamCount();
		for(size_t i=0; i<nstreams; i++)
		{
			StreamDescriptor desc(chan, i);

			auto item = Gtk::manage(new Gtk::MenuItem(desc.GetName(), false));
			item->signal_activate().connect(
				sigc::bind<StreamDescriptor>(sigc::mem_fun(*this, &OscilloscopeWindow::OnAddChannel), desc));
			m_channelsMenu.append(*item);
		}
	}

	m_channelsMenu.show_all();
}

/**
	@brief Refresh the trigger menu when we connect to a new instrument
 */
void OscilloscopeWindow::RefreshTriggerMenu()
{
	//Remove the old items
	auto children = m_setupTriggerMenu.get_children();
	for(auto c : children)
		m_setupTriggerMenu.remove(*c);

	for(auto scope : m_scopes)
	{
		auto item = Gtk::manage(new Gtk::MenuItem(scope->m_nickname, false));
		item->signal_activate().connect(
			sigc::bind<Oscilloscope*>(sigc::mem_fun(*this, &OscilloscopeWindow::OnTriggerProperties), scope));
		m_setupTriggerMenu.append(*item);
	}
}

/**
	@brief Refresh the export menu (for now, only done at startup)
 */
void OscilloscopeWindow::RefreshExportMenu()
{
	//Remove the old items
	auto children = m_exportMenu.get_children();
	for(auto c : children)
		m_exportMenu.remove(*c);

	vector<string> names;
	ExportWizard::EnumExportWizards(names);
	for(auto name : names)
	{
		auto item = Gtk::manage(new Gtk::MenuItem(name, false));
		item->signal_activate().connect(
			sigc::bind<std::string>(sigc::mem_fun(*this, &OscilloscopeWindow::OnExport), name));
		m_exportMenu.append(*item);
	}
}

/**
	@brief Update the protocol analyzer menu when we create or destroy an analyzer
 */
void OscilloscopeWindow::RefreshAnalyzerMenu()
{
	//Remove the old items
	auto children = m_windowAnalyzerMenu.get_children();
	for(auto c : children)
		m_windowAnalyzerMenu.remove(*c);

	//Add new ones
	for(auto a : m_analyzers)
	{
		auto item = Gtk::manage(new Gtk::MenuItem(a->GetDecoder()->GetDisplayName(), false));
		item->signal_activate().connect(
			sigc::bind<ProtocolAnalyzerWindow*>(sigc::mem_fun(*this, &OscilloscopeWindow::OnShowAnalyzer), a ));
		m_windowAnalyzerMenu.append(*item);
	}

	m_windowAnalyzerMenu.show_all();
}

/**
	@brief Update the multimeter menu when we load a new session
 */
void OscilloscopeWindow::RefreshMultimeterMenu()
{
	//Remove the old items
	auto children = m_windowMultimeterMenu.get_children();
	for(auto c : children)
		m_windowMultimeterMenu.remove(*c);

	//Add new stuff
	//TODO: support pure multimeters
	for(auto scope : m_scopes)
	{
		auto meter = dynamic_cast<Multimeter*>(scope);
		if(!meter)
			continue;

		auto item = Gtk::manage(new Gtk::MenuItem(meter->m_nickname, false));
		item->signal_activate().connect(
			sigc::bind<Multimeter*>(sigc::mem_fun(*this, &OscilloscopeWindow::OnShowMultimeter), meter ));
		m_windowMultimeterMenu.append(*item);
	}
}

void OscilloscopeWindow::OnShowAnalyzer(ProtocolAnalyzerWindow* window)
{
	window->show();
}

void OscilloscopeWindow::OnShowMultimeter(Multimeter* meter)
{
	//Did we have a dialog for the meter already?
	if(m_meterDialogs.find(meter) != m_meterDialogs.end())
		m_meterDialogs[meter]->show();

	//Need to create it
	else
	{
		auto dlg = new MultimeterDialog(meter);
		m_meterDialogs[meter] = dlg;
		dlg->show();
	}
}

bool OscilloscopeWindow::on_key_press_event(GdkEventKey* key_event)
{
	//Hotkeys for various special commands.
	//TODO: make this configurable

	switch(key_event->keyval)
	{
		case GDK_KEY_TouchpadToggle:
			OnStartSingle();
			break;

		default:
			break;
	}

	return true;
}

/**
	@brief Runs an export wizard
 */
void OscilloscopeWindow::OnExport(string format)
{
	//Stop triggering
	OnStop();

	//Make a list of all the channels (both scope channels and filters)
	vector<OscilloscopeChannel*> channels;
	auto filters = Filter::GetAllInstances();
	for(auto f : filters)
		channels.push_back(f);
	for(auto scope : m_scopes)
	{
		for(size_t i=0; i<scope->GetChannelCount(); i++)
			channels.push_back(scope->GetChannel(i));
	}

	//If we already have an export wizard, get rid of it
	if(m_exportWizard)
		delete m_exportWizard;

	//Run the actual wizard once we have a list of all channels we might want to export
	m_exportWizard = ExportWizard::CreateExportWizard(format, channels);
	m_exportWizard->show();
}

void OscilloscopeWindow::OnAboutDialog()
{
	Gtk::AboutDialog aboutDialog;

	aboutDialog.set_logo_default();
	aboutDialog.set_version(string("Version ") + GLSCOPECLIENT_VERSION);
	aboutDialog.set_copyright("Copyright © 2012-2022 Andrew D. Zonenberg and contributors");
	aboutDialog.set_license(
		"Redistribution and use in source and binary forms, with or without modification, "
		"are permitted provided that the following conditions are met:\n\n"
		"* Redistributions of source code must retain the above copyright notice, this list "
		"of conditions, and the following disclaimer.\n\n"
		"* Redistributions in binary form must reproduce the above copyright notice, this list "
		"of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.\n\n"
		"* Neither the name of the author nor the names of any contributors may be used to "
		"endorse or promote products derived from this software without specific prior written permission.\n\n"
		"THIS SOFTWARE IS PROVIDED BY THE AUTHORS \"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED "
		"TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL "
		"THE AUTHORS BE HELD LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES "
		"(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR "
		"BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT "
		"(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE "
		"POSSIBILITY OF SUCH DAMAGE.    "
	);
	aboutDialog.set_wrap_license(true);

	vector<Glib::ustring> authors
	{
		"9names",
		"Andres Manelli",
		"Andrew D. Zonenberg",
		"antikerneldev",
		"Benjamin Vernoux",
		"Dave Marples",
		"four0four",
		"Francisco Sedano",
		"Katharina B",
		"Kenley Cheung",
		"Mike Walters",
		"noopwafel",
		"Pepijn De Vos",
		"pd0wm"
		"randomplum",
		"rqou",
		"RX14",
		"sam210723",
		"smunaut",
		"tarunik",
		"Tom Verbeuere",
		"whitequark",
		"x44203"
	};
	aboutDialog.set_authors(authors);

	vector<Glib::ustring> artists
	{
		"Collateral Damage Studios"
	};
	aboutDialog.set_artists(artists);

	vector<Glib::ustring> hardware
	{
		"Andrew D. Zonenberg",
		"whitequark",
		"and several anonymous donors"
	};
	aboutDialog.add_credit_section("Hardware Contributions", hardware);

	aboutDialog.set_website("https://www.github.com/azonenberg/scopehal-apps");
	aboutDialog.set_website_label("Visit us on GitHub");

	aboutDialog.run();
}

void OscilloscopeWindow::OnFilterGraph()
{
	if(!m_graphEditor)
	{
		m_graphEditor = new FilterGraphEditor(this);
		m_graphEditor->Refresh();
		m_graphEditor->show();
	}
	else if(m_graphEditor->is_visible())
		m_graphEditor->hide();
	else
	{
		m_graphEditor->Refresh();
		m_graphEditor->show();
	}
}

void OscilloscopeWindow::LoadRecentlyUsedList()
{
	try
	{
		auto docs = YAML::LoadAllFromFile(m_preferences.GetConfigDirectory() + "/recent.yml");
		if(docs.empty())
			return;
		auto node = docs[0];

		for(auto it : node)
		{
			auto inst = it.second;
			m_recentlyUsed[inst["path"].as<string>()] = inst["timestamp"].as<long long>();
		}
	}
	catch(const YAML::BadFile& ex)
	{
		LogDebug("Unable to open recently used instruments file\n");
		return;
	}

}

void OscilloscopeWindow::SaveRecentlyUsedList()
{
	auto path = m_preferences.GetConfigDirectory() + "/recent.yml";
	FILE* fp = fopen(path.c_str(), "w");

	for(auto it : m_recentlyUsed)
	{
		auto nick = it.first.substr(0, it.first.find(":"));
		fprintf(fp, "%s:\n", nick.c_str());
		fprintf(fp, "    path: \"%s\"\n", it.first.c_str());
		fprintf(fp, "    timestamp: %ld\n", it.second);
	}

	fclose(fp);
}

void OscilloscopeWindow::AddCurrentToRecentlyUsedList()
{
	//Add our current entry to the recently-used list
	auto now = time(NULL);
	for(auto scope : m_scopes)
	{
		//Skip any mock scopes as they're not real things we can connect to
		if(dynamic_cast<MockOscilloscope*>(scope) != NULL)
			continue;

		string connectionString =
			scope->m_nickname + ":" +
			scope->GetDriverName() + ":" +
			scope->GetTransportName() + ":" +
			scope->GetTransportConnectionString();

		m_recentlyUsed[connectionString] = now;
	}

	//Delete anything old
	const int maxRecentInstruments = 10;
	while(m_recentlyUsed.size() > maxRecentInstruments)
	{
		string oldestPath = "";
		time_t oldestTime = now;

		for(auto it : m_recentlyUsed)
		{
			if(it.second < oldestTime)
			{
				oldestTime = it.second;
				oldestPath = it.first;
			}
		}

		m_recentlyUsed.erase(oldestPath);
	}
}

void OscilloscopeWindow::RefreshInstrumentMenu()
{
	//Remove the old items
	auto children = m_recentInstrumentsMenu.get_children();
	for(auto c : children)
		m_recentInstrumentsMenu.remove(*c);

	//Make a reverse mapping
	std::map<time_t, string> reverseMap;
	for(auto it : m_recentlyUsed)
		reverseMap[it.second] = it.first;

	//Sort the list by most recent
	vector<time_t> timestamps;
	for(auto it : m_recentlyUsed)
		timestamps.push_back(it.second);
	std::sort(timestamps.begin(), timestamps.end());

	//Add new ones
	for(int i=timestamps.size()-1; i>=0; i--)
	{
		auto t = timestamps[i];
		auto path = reverseMap[t];
		auto nick = path.substr(0, path.find(":"));

		auto item = Gtk::manage(new Gtk::MenuItem(nick, false));
		item->signal_activate().connect(
			sigc::bind<std::string>(sigc::mem_fun(*this, &OscilloscopeWindow::ConnectToScope), path));
		m_recentInstrumentsMenu.append(*item);
	}

	m_recentInstrumentsMenu.show_all();
}

/**
	@brief Search our set of oscilloscopes to see which ones have function generator capability
 */
void OscilloscopeWindow::FindScopeFuncGens()
{
	for(auto scope : m_scopes)
	{
		if((scope->GetInstrumentTypes() & Instrument::INST_FUNCTION) != Instrument::INST_FUNCTION)
			continue;
		m_funcgens.push_back(dynamic_cast<FunctionGenerator*>(scope));
	}
}

/**
	@brief Refresh the menu of available signal generators
 */
void OscilloscopeWindow::RefreshGeneratorsMenu()
{
	//Remove the old items
	auto children = m_windowGeneratorMenu.get_children();
	for(auto c : children)
		m_windowGeneratorMenu.remove(*c);

	//Add new stuff
	for(auto gen : m_funcgens)
	{
		auto item = Gtk::manage(new Gtk::MenuItem(gen->m_nickname, false));
		item->signal_activate().connect(
			sigc::bind<FunctionGenerator*>(sigc::mem_fun(*this, &OscilloscopeWindow::OnShowFunctionGenerator), gen));
		m_windowGeneratorMenu.append(*item);
	}
}

void OscilloscopeWindow::OnShowFunctionGenerator(FunctionGenerator* gen)
{
	//Did we have a dialog for the meter already?
	if(m_functionGeneratorDialogs.find(gen) != m_functionGeneratorDialogs.end())
		m_functionGeneratorDialogs[gen]->show();

	//Need to create it
	else
	{
		auto dlg = new FunctionGeneratorDialog(gen);
		m_functionGeneratorDialogs[gen] = dlg;
		dlg->show();
	}
}
