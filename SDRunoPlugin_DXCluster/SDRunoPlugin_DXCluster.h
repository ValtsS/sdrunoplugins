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
	void UpdateSpanCache();

	int DXCount() { return dxCount; }
	std::string GetStatusDetail();

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
	int checkLocalFreq(long long freq, std::string callsign);

	std::string cAddr;
	std::string cPort;
	std::string cCallsign;
	std::string cResponse;
	int timeMinutes;
	int dxCount;
	int sampleRate;
	bool responseMatched;
	SOCKET m_sock;

	// Diagnostics: append-only log in %APPDATA%\SDRplay plus a short
	// last-status string surfaced in the form's status label.
	void Log(const std::string& msg);
	void SetStatus(const std::string& s);
	std::string GetLogPath() { return m_logPath; }
	std::mutex m_logLock;
	std::string m_logPath;
	std::string m_lastStatus;
	long long m_statRecvBytes;
	long long m_statLines;
	long long m_statDx;
	long long m_statParseFail;
	long long m_statAnnotatorCalls;
	// SP1 power span cache. Refreshed by UpdateSpanCache() from the
	// plugin's own timer thread - the AnnotatorProcess hot path (which
	// runs on SDRuno's thread) only READS these, never calls the
	// controller. Set m_spanValid=false when the values are unusable.
	int m_sp1MinPwrC;
	int m_sp1MaxPwrC;
	long long m_sp1MinFreqC;
	long long m_sp1MaxFreqC;
	bool m_spanValid;
};