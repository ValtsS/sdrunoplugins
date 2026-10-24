#include <sstream>
#include <unoevent.h>
#include <iunoplugincontroller.h>
#include <vector>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include <cctype>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

#pragma comment(lib, "ws2_32.lib") // Winsock library

#include "SDRunoPlugin_DXCluster.h"
#include "SDRunoPlugin_DXClusterUi.h"

#define MAXBUF 4096
#define bzero(x,y) memset(x,0,y);

SDRunoPlugin_DXCluster::SDRunoPlugin_DXCluster(IUnoPluginController& controller) :
	IUnoPlugin(controller),
	m_form(*this, controller),
	m_worker(nullptr),
	head(NULL),
	tail(NULL),
	current(NULL),
	prev(NULL),
	freqcurr(NULL),
	annocurr(NULL),
	dxCount(0),
	sampleRate(2000000),
	m_started(false),
	m_sock(INVALID_SOCKET),
	responseMatched(false),
	m_statRecvBytes(0),
	m_statLines(0),
	m_statDx(0),
	m_statParseFail(0),
	m_statAnnotatorCalls(0),
	m_baselineC(-70),
	m_colourIdxC(1),
	m_orderTopDownC(false)
{
	const char* appdata = getenv("APPDATA");
	if (appdata != NULL && appdata[0] != 0)
	{
		m_logPath = std::string(appdata) + "\\SDRplay\\SDRunoPlugin_DXCluster.log";
	}
	else
	{
		m_logPath = "SDRunoPlugin_DXCluster.log";
	}
}

SDRunoPlugin_DXCluster::~SDRunoPlugin_DXCluster()
{
	// Belt-and-braces teardown: SDRuno should have driven Stop via the
	// form/ClosingDown path, but never destroy while the worker runs.
	m_started = false;
	if (m_sock != INVALID_SOCKET)
	{
#ifdef _WIN32
		shutdown(m_sock, SD_BOTH);
		closesocket(m_sock);
#else
		shutdown(m_sock, SHUT_RDWR);
		close(m_sock);
#endif
		m_sock = INVALID_SOCKET;
	}
	if (m_worker != nullptr)
	{
		if (m_worker->joinable())
		{
			m_worker->join();
		}
		delete m_worker;
		m_worker = nullptr;
	}
	clearList();
}

void SDRunoPlugin_DXCluster::AnnotatorProcess(std::vector<IUnoAnnotatorItem>& items)
{
	// Runs on SDRuno's thread at redraw rate. Deliberate constraints that
	// all hangs taught us: no controller calls, no widget reads (cached
	// copies only), no exceptions out, bounded output. Everything cross-
	// thread arrives via m_lock-protected snapshot + cache.
	try
	{
		const uint32_t colors[] = {
			0x00ff0000, // red
			0x00ffff00, // yellow
			0x00ff00ff, // purple
			0x00aaff00, // green
			0x0030a0ff, // light blue
			0x00ffffff  // white
		};

		struct SnapEntry { long long freq; std::string callsign; };
		std::vector<SnapEntry> snap;
		int baseline;
		int colourIdx;
		bool topDown;
		m_statAnnotatorCalls++;
		bool logThis = (m_statAnnotatorCalls == 1 || m_statAnnotatorCalls % 120 == 0);
		{
			std::lock_guard<std::mutex> l(m_lock);
			for (DXEntry* e = head; e != NULL; e = e->next)
			{
				SnapEntry s;
				s.freq = e->freq;
				s.callsign = e->callsign;
				snap.push_back(s);
			}
			baseline = m_baselineC;
			colourIdx = m_colourIdxC;
			topDown = m_orderTopDownC;
		}
		uint32_t rgb = colors[colourIdx];
		// Anchor + direction, no screen clamping (we deliberately query no
		// SP1 state anywhere - see UpdateSpanCache). Bottom-up (original):
		// climb from baseline. Top-down: same algorithm hung from the
		// baseline downward instead.
		int anchor = baseline;
		int window;
		if (sampleRate >= 2000000)
		{
			window = (int)(sampleRate / 100.);
		}
		else
		{
			window = (int)(sampleRate / 20.);
		}
		std::vector<long long> drawn;
		drawn.reserve(snap.size());
		for (size_t idx = 0; idx < snap.size(); idx++)
		{
			IUnoAnnotatorItem im;
			im.frequency = snap[idx].freq;
			im.text = snap[idx].callsign;
			// Overlap count against drawn neighbours only, so filtered-out
			// (off-screen) spots don't push visible labels upward.
			int count = 0;
			for (size_t j = 0; j < drawn.size(); j++)
			{
				if (llabs(snap[idx].freq - drawn[j]) < window)
				{
					count++;
				}
			}
			int pwr = topDown ? (anchor - count * 5) : (anchor + count * 5);
			im.power = pwr;
			im.rgb = rgb;
			// Degenerate zero-length anchor at the marker itself: every
			// field carries a real in-span value, so no projection in
			// SDRuno's renderer ever sees 0 Hz or off-scale dB from us.
			// (Previously this was (0, -1000); the ADSB reference plugin
			// never submits degenerate anchors, and our hangs only ever
			// started once real non-empty lists reached the renderer.)
			im.lineToFreq = im.frequency;
			im.lineToPower = im.power;
			if (count == 0)
			{
				im.style = IUnoAnnotatorStyle::AnnotatorStyleMarkerAndLine;
			}
			else
			{
				im.style = IUnoAnnotatorStyle::AnnotatorStyleMarker;
			}

			items.push_back(im);
			drawn.push_back(snap[idx].freq);
		}
		dxCount = (int)snap.size();
		if (logThis)
		{
			Log("annotator call #" + std::to_string(m_statAnnotatorCalls) +
				" items=" + std::to_string(items.size()) +
				" dx=" + std::to_string(m_statDx) +
				" order=" + std::string(topDown ? "top-down" : "bottom-up") +
				std::string(snap.size() > 40 ? " capped" : ""));
		}
	}
	catch (...)
	{
		// Never let an exception escape into SDRuno.
		try { Log("annotator exception swallowed"); } catch (...) {}
	}
}

void SDRunoPlugin_DXCluster::StartAnnotator()
{
	m_controller.RegisterAnnotator(this);
}

void SDRunoPlugin_DXCluster::StopAnnotator()
{
	m_controller.UnregisterAnnotator(this);
}

void SDRunoPlugin_DXCluster::StartDXCluster(std::string addr, std::string port, std::string callsign, int timeMins, std::string response)
{
	cAddr = addr;
	cPort = port;
	cCallsign = callsign;
	timeMinutes = timeMins;
	cResponse = response;

	std::lock_guard<std::mutex> l(m_lock);
	if (m_started)
	{
		return;
	}

	Log("=== start " + addr + ":" + port + " timer=" + std::to_string(timeMins) + "min ===");
	m_started = true;
	responseMatched = false;
	UpdateSampleRate();
	StartAnnotator();
	m_worker = new std::thread(&SDRunoPlugin_DXCluster::WorkerFunction, this);
}

void SDRunoPlugin_DXCluster::StopDXCluster()
{
	{
		std::lock_guard<std::mutex> l(m_lock);
		if (!m_started)
		{
			return;
		}
		m_started = false;
	}

	StopAnnotator();
	// Unblock the worker's recv(): shutdown() wakes it, close frees it.
	// Done outside m_lock so WorkerFunction's short critical sections
	// can't deadlock against us.
	if (m_sock != INVALID_SOCKET)
	{
#ifdef _WIN32
		shutdown(m_sock, SD_BOTH);
		closesocket(m_sock);
#else
		shutdown(m_sock, SHUT_RDWR);
		close(m_sock);
#endif
		m_sock = INVALID_SOCKET;
	}
	if (m_worker != nullptr)
	{
		if (m_worker->joinable())
		{
			m_worker->join();
		}
		delete m_worker;
		m_worker = nullptr;
	}
	Log("=== stopped ===");
	SetStatus("stopped");
}

void SDRunoPlugin_DXCluster::HandleEvent(const UnoEvent& ev)
{
	m_form.HandleEvent(ev);	
}

void SDRunoPlugin_DXCluster::Log(const std::string& msg)
{
	std::lock_guard<std::mutex> l(m_logLock);
	if (m_logPath.empty())
	{
		return;
	}
	SYSTEMTIME st;
	GetLocalTime(&st);
	char ts[32];
	snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	FILE* f = NULL;
	fopen_s(&f, m_logPath.c_str(), "a");
	if (f != NULL)
	{
		fprintf(f, "[%s] %s\n", ts, msg.c_str());
		fclose(f);
	}
}

void SDRunoPlugin_DXCluster::SetStatus(const std::string& s)
{
	std::lock_guard<std::mutex> l(m_logLock);
	m_lastStatus = s;
}

std::string SDRunoPlugin_DXCluster::GetStatusDetail()
{
	std::lock_guard<std::mutex> l(m_logLock);
	return m_lastStatus;
}

// read data until needle is seen (optionally case-insensitive)
bool SDRunoPlugin_DXCluster::waitforstring(SOCKET sockfd, const char *s, bool caseInsensitive)
{
	int res, pos = 0;
	char buffer[MAXBUF];
	bzero(buffer, MAXBUF);

	std::string needle(s ? s : "");
	if (caseInsensitive)
	{
		for (size_t i = 0; i < needle.size(); i++)
		{
			needle[i] = (char)tolower((unsigned char)needle[i]);
		}
	}

	if (responseMatched)
	{
		return true;
	}

	while ((res = recv(sockfd, buffer + pos, sizeof(buffer) - pos - 1, 0)) > 0)
	{
		if (!m_started)
		{
			return false;
		}

		if (responseMatched)
		{
			return true;
		}

		pos += res;
		buffer[pos] = 0;
#ifdef DEBUG
		char outbuf[MAXBUF];
		snprintf(outbuf, sizeof(outbuf), "SDRuno wait %s", buffer);
		OutputDebugStringA(outbuf);
#endif
		bool found;
		if (caseInsensitive)
		{
			std::string hay(buffer, pos);
			for (size_t i = 0; i < hay.size(); i++)
			{
				hay[i] = (char)tolower((unsigned char)hay[i]);
			}
			found = (hay.find(needle) != std::string::npos);
		}
		else
		{
			found = (strstr(buffer, s) != NULL);
		}
		if (found)
		{
#ifdef DEBUG
			OutputDebugStringA("SDRuno Response Matched");
#endif
			responseMatched = true;
			return true;
		}
		if (pos >= (int)sizeof(buffer) - 1)
		{
			// Needle not in a full buffer; keep the tail in case it
			// straddles the boundary, drop the rest.
			size_t keep = needle.size();
			if (keep > (size_t)pos)
			{
				keep = pos;
			}
			memmove(buffer, buffer + pos - keep, keep);
			pos = (int)keep;
		}
	}
	return false;
}

void SDRunoPlugin_DXCluster::sendstring(SOCKET sockfd, const char *s)
{
	if (s == NULL)
	{
		return;
	}
	size_t len = strlen(s);
	size_t sent = 0;
	while (sent < len)
	{
		int res = send(sockfd, s + sent, (int)(len - sent), 0);
		if (res <= 0)
		{
			break;
		}
		sent += res;
	}
}

// Post-login response wait. Unlike waitforstring it cannot hang forever:
// after timeoutSec it proceeds anyway (some servers, e.g. SpotMux, only
// banner the response marker BEFORE the login prompt). DX lines seen while
// waiting are dispatched immediately so the connect burst is not lost.
// Returns true while the socket is alive, false when it died.
bool SDRunoPlugin_DXCluster::waitForResponse(SOCKET sockfd, const std::string& needle, int timeoutSec)
{
	std::string want = needle;
	for (size_t i = 0; i < want.size(); i++)
	{
		want[i] = (char)tolower((unsigned char)want[i]);
	}
	std::string hay;
	hay.reserve(8192);
	std::string pending;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
	char buf[1024];

	while (m_started && !responseMatched)
	{
		auto now = std::chrono::steady_clock::now();
		if (now >= deadline)
		{
			Log("response '" + needle + "' not seen in " + std::to_string(timeoutSec) + "s, proceeding anyway");
			responseMatched = true;
			return true;
		}
		int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
		fd_set rf;
		FD_ZERO(&rf);
		FD_SET(sockfd, &rf);
		timeval tv;
		tv.tv_sec = ms / 1000;
		tv.tv_usec = (ms % 1000) * 1000;
		int r = select((int)sockfd + 1, &rf, NULL, NULL, &tv);
		if (r == 0)
		{
			continue; // re-check deadline / m_started
		}
		if (r < 0)
		{
			return false;
		}
		int res = recv(sockfd, buf, (int)sizeof(buf) - 1, 0);
		if (res <= 0)
		{
			return false;
		}
		m_statRecvBytes += res;
		pending.append(buf, res);
		size_t nl;
		while ((nl = pending.find('\n')) != std::string::npos)
		{
			std::string line = pending.substr(0, nl);
			pending.erase(0, nl + 1);
			if (!line.empty() && line.back() == '\r')
			{
				line.pop_back();
			}
			m_statLines++;
			hay += line;
			hay += '\n';
			if (hay.size() > 8192)
			{
				hay.erase(0, hay.size() - 8192);
			}
			if (line.find("DX de") != std::string::npos)
			{
				processDX(line.c_str());
			}
		}
		std::string lower = hay;
		for (size_t i = 0; i < lower.size(); i++)
		{
			lower[i] = (char)tolower((unsigned char)lower[i]);
		}
		if (lower.find(want) != std::string::npos)
		{
			responseMatched = true;
			Log("response '" + needle + "' matched");
			return true;
		}
	}
	return m_started ? true : false;
}

void SDRunoPlugin_DXCluster::WorkerFunction()
{
	int port;
	try
	{
		size_t pos = 0;
		port = stoi(cPort, &pos);
		if (pos != cPort.size() || port <= 0 || port > 65535)
		{
			Log("bad port '" + cPort + "'");
			SetStatus("bad port");
			m_started = false;
			return;
		}
	}
	catch (...)
	{
		Log("bad port '" + cPort + "'");
		SetStatus("bad port");
		m_started = false;
		return;
	}

	Log("connecting to " + cAddr + ":" + std::to_string(port) + " as '" + cCallsign + "'");
	SetStatus("connecting");

#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		m_started = false;
		return;
	}
#endif

	SOCKET s = INVALID_SOCKET;
	struct addrinfo hints;
	struct addrinfo* addrs = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	char portStr[16];
	snprintf(portStr, sizeof(portStr), "%d", port);
	if (getaddrinfo(cAddr.c_str(), portStr, &hints, &addrs) != 0 || addrs == NULL)
	{
#ifdef _WIN32
		WSACleanup();
#endif
		Log("DNS failed for '" + cAddr + "'");
		SetStatus("DNS failed");
		m_started = false;
		return;
	}

	s = socket(addrs->ai_family, addrs->ai_socktype, addrs->ai_protocol);
#ifdef _WIN32
	if (s == INVALID_SOCKET)
	{
		freeaddrinfo(addrs);
		WSACleanup();
		m_started = false;
		return;
	}
#else
	if (s < 0)
	{
		freeaddrinfo(addrs);
		m_started = false;
		return;
	}
#endif

	// Publish early so StopDXCluster can abort even a stuck connect().
	{
		std::lock_guard<std::mutex> l(m_lock);
		m_sock = s;
	}

	// Connect to server with a timeout. A blocking connect() has no bound
	// (SYN retransmits for minutes on a black-holed route) and Stop cannot
	// rescue it before m_sock is assigned, so the join in StopDXCluster
	// would wedge the panel. Non-blocking + select() caps this at 10 s.
	bool connected = false;
	int connErr = 0;
#ifdef _WIN32
	u_long nb = 1;
	if (ioctlsocket(s, FIONBIO, &nb) == 0)
	{
		if (connect(s, addrs->ai_addr, (int)addrs->ai_addrlen) == 0)
		{
			connected = true;
		}
		else if (WSAGetLastError() == WSAEWOULDBLOCK)
		{
			fd_set wf;
			FD_ZERO(&wf);
			FD_SET(s, &wf);
			timeval tv;
			tv.tv_sec = 10;
			tv.tv_usec = 0;
			int r = select((int)s + 1, NULL, &wf, NULL, &tv);
			if (r > 0 && FD_ISSET(s, &wf))
			{
				int soerr = 0;
				int optlen = sizeof(soerr);
				if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &optlen) == 0 && soerr == 0)
				{
					connected = true;
				}
				else
				{
					connErr = soerr;
				}
			}
			else if (r == 0)
			{
				connErr = WSAETIMEDOUT;
			}
			else
			{
				connErr = WSAGetLastError();
			}
		}
		else
		{
			connErr = WSAGetLastError();
		}
		nb = 0;
		ioctlsocket(s, FIONBIO, &nb);
	}
	else
	{
		connErr = WSAGetLastError();
	}
#else
	if (connect(s, addrs->ai_addr, (int)addrs->ai_addrlen) == 0)
	{
		connected = true;
	}
	else
	{
		connErr = errno;
	}
#endif
	if (!connected)
	{
		{
			std::lock_guard<std::mutex> l(m_lock);
			if (m_sock == s)
			{
				m_sock = INVALID_SOCKET;
			}
		}
#ifdef _WIN32
		closesocket(s);
		WSACleanup();
#else
		close(s);
#endif
		freeaddrinfo(addrs);
		Log("connect failed to " + cAddr + ":" + std::to_string(port) + " err=" + std::to_string(connErr));
		SetStatus("conn failed " + std::to_string(connErr));
		m_started = false;
		return;
	}
	freeaddrinfo(addrs);
	Log("connected to " + cAddr + ":" + std::to_string(port));
	SetStatus("connected");

	{
		std::lock_guard<std::mutex> l(m_lock);
		if (!m_started)
		{
			// Stopped while connecting; StopDXCluster already closed it.
			return;
		}
	}

	// Login to Telnet Server
	if (cCallsign.length() > 1)
	{
		if (waitforstring(s, "ogin: ", false))
		{
			Log("login prompt seen");
		}
		else
		{
			Log("login prompt NOT seen (stopped or socket dead)");
		}
		responseMatched = false;
		char csTmp[64];
		snprintf(csTmp, sizeof(csTmp), "%s\r\n", cCallsign.substr(0, sizeof(csTmp) - 3).c_str());
		sendstring(s, csTmp);
		Log("callsign sent");
		if (cResponse.length() > 1)
		{
			// Case-insensitive, 10 s timeout, DX lines seen while
			// waiting are still processed. SpotMux-style servers only
			// banner the marker before the prompt, so without the
			// timeout+proceed this gate never opens (DX count 0).
			std::string resp = cResponse.substr(0, 200);
			if (waitForResponse(s, resp, 10))
			{
				if (!m_started)
				{
					Log("stopped during response wait");
					SetStatus("stopped");
				}
				// waitForResponse already logged matched vs timeout.
			}
			else
			{
				Log("socket died during response wait");
				SetStatus("socket lost");
				m_started = false;
			}
		}
		else
		{
			responseMatched = true;
			Log("no response string configured, skipping wait");
		}
	}
	else
	{
		responseMatched = true;
		Log("no callsign configured, skipping login");
	}
	SetStatus("receiving");

	int res;
	char buffer[MAXBUF];
	std::string pending;
	long long lastLoggedDx = 0;

	// Now process DX records
	while (m_started && responseMatched)
	{
		bzero(buffer, MAXBUF);
		res = recv(s, buffer, sizeof(buffer) - 1, 0);

		if (res == 0)
		{
			// Orderly shutdown by the server.
			Log("server closed connection");
			SetStatus("server closed");
			m_started = false;
			break;
		}

		if (res < 0)
		{
#ifdef _WIN32
			if (WSAGetLastError() == WSAEINTR)
			{
				continue;
			}
#endif
			Log("recv error, stopping");
			SetStatus("recv error");
			m_started = false;
			break;
		}

		if (!m_started)
		{
			break;
		}

		buffer[res] = 0;
		m_statRecvBytes += res;
#ifdef DEBUG
		char out[MAXBUF];
		snprintf(out, sizeof(out), "SDRuno in %s", buffer);
		OutputDebugStringA(out);
#endif
		// One recv() can hold many lines (connect burst) or half a
		// line; split on newlines and dispatch complete lines only.
		pending.append(buffer, res);
		size_t nl;
		while ((nl = pending.find('\n')) != std::string::npos)
		{
			std::string line = pending.substr(0, nl);
			pending.erase(0, nl + 1);
			if (!line.empty() && line.back() == '\r')
			{
				line.pop_back();
			}
			m_statLines++;
			if (line.find("DX de") != std::string::npos)
			{
				processDX(line.c_str());
				if (m_statDx - lastLoggedDx >= 25)
				{
					lastLoggedDx = m_statDx;
					Log("stats bytes=" + std::to_string(m_statRecvBytes) +
						" lines=" + std::to_string(m_statLines) +
						" dx=" + std::to_string(m_statDx) +
						" parseFail=" + std::to_string(m_statParseFail));
				}
			}
		}
		if (pending.size() > 65536)
		{
			// Never saw a newline; drop garbage rather than grow forever.
			pending.clear();
		}
	}
	Log("recv loop ended: bytes=" + std::to_string(m_statRecvBytes) +
		" lines=" + std::to_string(m_statLines) +
		" dx=" + std::to_string(m_statDx) +
		" parseFail=" + std::to_string(m_statParseFail));
	if (m_started)
	{
		SetStatus("stopped");
	}
	{
		std::lock_guard<std::mutex> l(m_lock);
		if (m_sock == s)
		{
			m_sock = INVALID_SOCKET;
		}
	}
#ifdef _WIN32
	closesocket(s);
	WSACleanup();
#else
	close(s);
#endif
}

// Token-based parse of one "DX de ..." line. Robust to the variable
// spotter-callsign width that breaks fixed-column slicing, e.g.:
//   DX de UT7DP:       7128.1  9A/S52DG/P   TNX QSO 73!   0732Z
//   DX de DL4CH:    10489710.0  DF7DQ       qo 100        0853Z
// Grammar: "DX de" SPOTTER":" FREQ_KHZ DXCALL ... HHTMM"Z".
// Returns false if the line does not match (caller drops it).
bool SDRunoPlugin_DXCluster::TokenParseDX(const std::string& line, std::string& callsign, std::string& timeUTC, long long& freqHz)
{
	callsign.clear();
	timeUTC.clear();
	freqHz = 0;

	size_t dxdepos = line.find("DX de");
	if (dxdepos == std::string::npos)
	{
		return false;
	}
	std::string rest = line.substr(dxdepos + 5);
	// split on whitespace
	std::vector<std::string> toks;
	{
		size_t i = 0;
		while (i < rest.size())
		{
			while (i < rest.size() && isspace((unsigned char)rest[i]))
			{
				i++;
			}
			if (i >= rest.size())
			{
				break;
			}
			size_t j = i;
			while (j < rest.size() && !isspace((unsigned char)rest[j]))
			{
				j++;
			}
			toks.push_back(rest.substr(i, j - i));
			i = j;
		}
	}
	// toks[0] = spotter "CALL:", toks[1] = freq kHz, toks[2] = dx call
	if (toks.size() < 3)
	{
		return false;
	}
	if (toks[0].empty() || toks[0].back() != ':')
	{
		return false;
	}
	double dfreqkHz;
	try
	{
		size_t used = 0;
		dfreqkHz = stod(toks[1], &used);
		if (used != toks[1].size())
		{
			return false;
		}
	}
	catch (...)
	{
		return false;
	}
	if (toks[2].empty())
	{
		return false;
	}
	// Spot time: first token shaped NNNNZ after the dx call (comment
	// length varies, so never use a fixed offset).
	std::string hhmm;
	for (size_t k = 3; k < toks.size(); k++)
	{
		const std::string& t = toks[k];
		if (t.size() == 5 && t[4] == 'Z' &&
			isdigit((unsigned char)t[0]) && isdigit((unsigned char)t[1]) &&
			isdigit((unsigned char)t[2]) && isdigit((unsigned char)t[3]))
		{
			hhmm = t.substr(0, 4);
			break;
		}
	}
	if (hhmm.empty())
	{
		return false;
	}

	freqHz = (long long)(dfreqkHz * 1000);
	callsign = toks[2];
	timeUTC = hhmm;
	return true;
}

void SDRunoPlugin_DXCluster::processDX(const char *s)
{
	if (s == NULL)
	{
#ifdef DEBUG
		OutputDebugStringA("SDRuno processDX null string");
#endif
		return;
	}
#ifdef DEBUG
	char out[MAXBUF];
	snprintf(out, sizeof(out), "SDRuno pr %s", s);
	OutputDebugStringA(out);
#endif
	// s may hold several lines (connect burst) or a fragment; handle
	// each complete "DX de" line, keep behaviour for single lines.
	std::string data(s);
	size_t start = 0;
	bool any = false;
	while (true)
	{
		size_t nl = data.find('\n', start);
		std::string line = (nl == std::string::npos) ? data.substr(start) : data.substr(start, nl - start);
		if (!line.empty() && line.back() == '\r')
		{
			line.pop_back();
		}
		if (line.find("DX de") != std::string::npos)
		{
			processDXLine(line);
			any = true;
		}
		if (nl == std::string::npos)
		{
			break;
		}
		start = nl + 1;
	}
	if (!any && data.find("DX de") != std::string::npos)
	{
		// No newline present but DX content exists (single line).
		processDXLine(data);
	}
}

void SDRunoPlugin_DXCluster::processDXLine(const std::string& line)
{
	std::string scsign, stimeZ;
	long long llfreqHz = 0;
	if (!TokenParseDX(line, scsign, stimeZ, llfreqHz))
	{
		m_statParseFail++;
		if (m_statParseFail <= 3)
		{
			Log("parse fail: '" + line.substr(0, 120) + "'");
		}
#ifdef DEBUG
		OutputDebugStringA("SDRuno process line error");
#endif
		return;
	}
	m_statDx++;

	// Per-spot accept log: lets "where is call X on band Y?" checks be
	// answered by grepping the log. Volume is ~1 line/spot (burst is a
	// one-off, steady state is a few/min). Logged outside the list lock.
	int pruned = 0;
	{
		std::lock_guard<std::mutex> l(m_lock);
		if (head != NULL)
		{
			// Prune duplicate-callsign and expired entries. Unlink AND delete;
			// keep prev on the surviving chain (old code advanced prev onto the
			// removed node and leaked it).
			DXEntry* p = head;
			DXEntry* prevEntry = NULL;
			while (p != NULL)
			{
				DXEntry* next = p->next;
				if (scsign.compare(p->callsign) == 0 || olderThanTimer(p->timeUTC))
				{
					if (prevEntry == NULL)
					{
						head = next;
					}
					else
					{
						prevEntry->next = next;
					}
					if (p == tail)
					{
						tail = prevEntry;
					}
					delete p;
					pruned++;
				}
				else
				{
					prevEntry = p;
				}
				p = next;
			}
		}

		add_DXEntry(scsign, stimeZ, llfreqHz);
	}
	Log("DX " + scsign + " " + std::to_string(llfreqHz) + "Hz " + stimeZ + "Z pruned=" + std::to_string(pruned));
}

void SDRunoPlugin_DXCluster::add_DXEntry(std::string callsign, std::string t, long long freq)
{
	DXEntry *tmp = new DXEntry;
	tmp->callsign = callsign;
	tmp->freq = freq;
	tmp->timeUTC = t;
	tmp->next = NULL;

	if (head == NULL)
	{
		head = tmp;
		tail = tmp;
	}
	else
	{
		tail->next = tmp;
		tail = tail->next;
	}
}

void SDRunoPlugin_DXCluster::clearList()
{
	std::lock_guard<std::mutex> l(m_lock);
	DXEntry* e = head;
	while (e != NULL)
	{
		DXEntry* next = e->next;
		delete e;
		e = next;
	}
	head = tail = current = prev = annocurr = freqcurr = NULL;
	dxCount = 0;
}

bool SDRunoPlugin_DXCluster::olderThanTimer(std::string timeUTC)
{
	std::string shour = timeUTC.substr(0, 2);
	std::string smin = timeUTC.substr(2, 2);
	int ihour = stoi(shour);
	int imin = stoi(smin);
	int totalMins = (ihour * 60) + imin;

	std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
	time_t tt = std::chrono::system_clock::to_time_t(now);
	tm utc_tm = *gmtime(&tt);
	int totalMinsUTC = (utc_tm.tm_hour * 60) + utc_tm.tm_min;

	// TODO: cope for 24 -> 0 hour case

	if (totalMinsUTC - totalMins > timeMinutes)
	{
		return true;
	}

	return false;
}

void SDRunoPlugin_DXCluster::UpdateSampleRate()
{
	sampleRate = (int)m_controller.GetSampleRate(0);
}

// Called ~1/s from the plugin's own form-timer thread. Deliberately queries
// no SP1/controller state (every build that did hung SDRuno within minutes);
// it only copies widget state into plain cached members for the hot path.
void SDRunoPlugin_DXCluster::RefreshUiCache()
{
	int baseline = -70;
	int colourIdx = 1;
	bool topDown = false;
	try { baseline = m_form.GetBaseline(); } catch (...) { }
	try { colourIdx = m_form.GetColourIndex(); } catch (...) { }
	try { topDown = m_form.GetOrderTopDown(); } catch (...) { }
	if (colourIdx < 0 || colourIdx > 5)
	{
		colourIdx = 1;
	}
	std::lock_guard<std::mutex> l(m_lock);
	m_baselineC = baseline;
	m_colourIdxC = colourIdx;
	m_orderTopDownC = topDown;
}