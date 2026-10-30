#pragma once

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#ifdef _WIN32
#include <winsock2.h>
#else
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#endif
#include <iunoplugincontroller.h>
#include <iunoplugin.h>
#include <iunostreamobserver.h>
#include <iunoaudioobserver.h>
#include <iunoaudioprocessor.h>
#include <iunostreamobserver.h>
#include <iunoannotator.h>

#include "SDRunoPlugin_DXClusterUi.h"

class SDRunoPlugin_DXCluster : public IUnoPlugin,
							   public IUnoAnnotator
{

public:
	
	SDRunoPlugin_DXCluster(IUnoPluginController& controller);
	virtual ~SDRunoPlugin_DXCluster();

	virtual const char* GetPluginName() const override { return "SDRuno DX Cluster Plugin"; }

	// IUnoPlugin
	virtual void HandleEvent(const UnoEvent& ev) override;

	virtual void AnnotatorProcess(std::vector<IUnoAnnotatorItem>& items) override;

	void StartDXCluster(std::string addr, std::string port, std::string callsign, int timeMins, std::string response);
	void StopDXCluster();
	bool IsRunning() { return m_started; }
	void StartAnnotator();
	void StopAnnotator();

	void UpdateSampleRate();
	// RefreshUiCache: copies widget state (baseline/colour/order) into
	// plain members. Called ~1/s from OUR form-timer thread (nana's own
	// thread - the only thread that may touch widgets). The annotator hot
	// path (SDRuno's thread) reads only these cached copies: never widgets,
	// never the controller.
	void RefreshUiCache();

	int DXCount() { return dxCount; }

	struct DXEntry {
		std::string callsign;
		long long freq;
		std::string timeUTC;
		IUnoAnnotatorStyle style;
		DXEntry *next;
	};

	DXEntry *head, *tail, *current, *prev, *annocurr, *freqcurr;

private:
	
	void WorkerFunction();
	std::thread* m_worker;
	std::mutex m_lock;
	SDRunoPlugin_DXClusterUi m_form;
	bool m_started;
	void sendstring(SOCKET sockfd, const char *s);
	bool waitforstring(SOCKET sockfd, const char *s, bool caseInsensitive);
	// Post-login wait with timeout: dispatches DX lines seen while waiting,
	// matches needle case-insensitively, proceeds on timeout (returns true
	// while the socket is alive, false when it died). Never hangs forever.
	bool waitForResponse(SOCKET sockfd, const std::string& needle, int timeoutSec);
	void processDX(const char *s);
	bool TokenParseDX(const std::string& line, std::string& callsign, std::string& timeUTC, long long& freqHz);
	void processDXLine(const std::string& line);
	void clearList();
	void add_DXEntry(std::string callsign, std::string t, long long freq);
	bool olderThanTimer(std::string timeUTC);
	// Canonical band anchor (Hz) for a spot frequency, used so the same
	// DX callsign is kept once per band (DX stations work several bands)
	// instead of once across all bands.
	static long long BandOf(long long freqHz);

	std::string cAddr;
	std::string cPort;
	std::string cCallsign;
	std::string cResponse;
	int timeMinutes;
	int dxCount;
	std::atomic<int> sampleRate;
	bool responseMatched;
	SOCKET m_sock;

	// Debug logging - OFF unless the marker file
	// %APPDATA%\SDRplay\DXClusterDebug.txt exists (drop it in, restart the
	// plugin). Logs to %APPDATA%\SDRplay\SDRunoPlugin_DXCluster.log.
	std::mutex m_logLock;
	std::string m_logPath;
	bool m_logEnabled;
	void Log(const std::string& msg);

	// Widget-state cache, see RefreshUiCache. Written on our timer thread,
	// read on SDRuno's annotator thread, always under m_lock.
	int m_baselineC;
	int m_colourIdxC;
	bool m_orderTopDownC;
};