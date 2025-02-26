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
	@brief Top-level application class
 */
#ifndef ScopeApp_h
#define ScopeApp_h

/**
	@brief The main application class
 */
class ScopeApp : public Gtk::Application
{
public:
	ScopeApp()
	 : Gtk::Application()
	 , m_terminating(false)
	 , m_window(NULL)
	{}

	virtual ~ScopeApp();

	std::vector<Oscilloscope*> ConnectToScopes(std::vector<std::string> scopes);

	virtual void run(
		std::vector<Oscilloscope*> scopes,
		std::vector<std::string> filesToLoad,
		bool reconnect,
		bool nodata,
		bool retrigger,
		bool nodigital,
		bool nospectrum);

	void DispatchPendingEvents();

	void ShutDownSession();

	bool IsTerminating()
	{ return m_terminating; }

	void StartScopeThreads(std::vector<Oscilloscope*> scopes);

protected:
	bool m_terminating;

	OscilloscopeWindow* m_window;

	std::vector<std::thread*> m_threads;
};

void ScopeThread(Oscilloscope* scope);

extern ScopeApp* g_app;

#endif
