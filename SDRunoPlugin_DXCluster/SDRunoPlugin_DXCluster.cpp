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
#include <mstcpip.h>
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
	m_logEnabled(false),
	m_baselineC(-70),
	m_colourIdxC(1),
	m_orderTopDownC(false)
{
	// Debug logging is opt-in: only enabled when the marker file exists,
	// so a stock build does zero file I/O until a tester drops it in.
	const char* appdata = getenv("APPDATA");
	if (appdata != NULL && appdata[0] != 0)
	{
		std::string dir = std::string(appdata) + "\\SDRplay";
		m_logPath = dir + "\\SDRunoPlugin_DXCluster.log";
		FILE* f = fopen((dir + "\\DXClusterDebug.txt").c_str(), "r");
		if (f != NULL)
		{
			fclose(f);
			m_logEnabled = true;
		}
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
		// Submit newest-first. The list is oldest-first (head) .. newest
		// (tail); SDRuno's annotator renders the head of the returned
		// vector and drops the rest, so a burst-sized oldest-first list
		// would push new spots (at the tail) past the render limit and
		// they'd never appear until reload. Reversing puts the newest
		// spots at the head, where they always render.
		for (size_t idx = snap.size(); idx-- > 0;)
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
	}
	catch (...)
	{
		// Never let an exception escape into SDRuno.
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
}

void SDRunoPlugin_DXCluster::HandleEvent(const UnoEvent& ev)
{
	m_form.HandleEvent(ev);	
}

void SDRunoPlugin_DXCluster::Log(const std::string& msg)
{
	if (!m_logEnabled || m_logPath.empty())
	{
		return;
	}
	std::lock_guard<std::mutex> l(m_logLock);
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
			m_started = false;
			return;
		}
	}
	catch (...)
	{
		m_started = false;
		return;
	}

#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		m_started = false;
		return;
	}
#endif

	// Outer retry loop: whenever the connection drops we clean up and try
	// again until the user presses Stop (which clears m_started).
	int attempt = 0;
	while (m_started)
	{
		attempt++;
		Log("worker: connecting to " + cAddr + ":" + std::to_string(port) + " (attempt " + std::to_string(attempt) + ")");

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
			Log("worker: DNS failed for '" + cAddr + "'");
			break;
		}

		s = socket(addrs->ai_family, addrs->ai_socktype, addrs->ai_protocol);
#ifdef _WIN32
		if (s == INVALID_SOCKET)
#else
		if (s < 0)
#endif
		{
			freeaddrinfo(addrs);
			Log("worker: socket creation failed");
			break;
		}

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
				}
			}
			nb = 0;
			ioctlsocket(s, FIONBIO, &nb);
		}
#else
		if (connect(s, addrs->ai_addr, (int)addrs->ai_addrlen) == 0)
		{
			connected = true;
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
#else
			close(s);
#endif
			freeaddrinfo(addrs);
			Log("worker: connect failed");
			// Keep retrying unless the user stopped.
			if (m_started)
			{
				for (int i = 0; i < 20 && m_started; i++)
				{
#ifdef _WIN32
					Sleep(250);
#else
					usleep(250000);
#endif
				}
			}
			continue;
		}
		freeaddrinfo(addrs);
		Log("worker: connected");

		// Detect silent deaths (NAT/firewall drop with no FIN/RST, which
		// would otherwise block recv forever and freeze the spot list).
		// Keepalive probes after 60 s idle, then every 5 s, so a dead link
		// surfaces as a recv error within about a minute and the retry loop
		// above reconnects.
#ifdef _WIN32
		{
			int one = 1;
			setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (char*)&one, sizeof(one));
			tcp_keepalive ka;
			ka.onoff = 1;
			ka.keepalivetime = 60000;      // 60 s idle before probing
			ka.keepaliveinterval = 5000;   // then every 5 s
			DWORD kwr = 0;
			WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), NULL, 0, &kwr, NULL, NULL);
		}
#endif

		{
			std::lock_guard<std::mutex> l(m_lock);
			if (!m_started)
			{
				// Stopped while connecting; StopDXCluster already closed it.
#ifdef _WIN32
				closesocket(s);
#else
				close(s);
#endif
				break;
			}
		}

		// Login to Telnet Server
		responseMatched = false;
		if (cCallsign.length() > 1)
		{
			waitforstring(s, "ogin: ", false);
			char csTmp[64];
			snprintf(csTmp, sizeof(csTmp), "%s\r\n", cCallsign.substr(0, sizeof(csTmp) - 3).c_str());
			sendstring(s, csTmp);
			if (cResponse.length() > 1)
			{
				// Case-insensitive, 10 s timeout, DX lines seen while
				// waiting are still processed. Some servers (e.g. SpotMux)
				// only banner the response marker before the prompt, so
				// without the timeout+proceed this gate never opens.
				std::string resp = cResponse.substr(0, 200);
				if (!waitForResponse(s, resp, 10))
				{
					Log("worker: login failed, reconnecting");
					{
						std::lock_guard<std::mutex> l(m_lock);
						if (m_sock == s)
						{
							m_sock = INVALID_SOCKET;
						}
					}
#ifdef _WIN32
					closesocket(s);
#else
					close(s);
#endif
					if (m_started)
					{
						for (int i = 0; i < 20 && m_started; i++)
						{
#ifdef _WIN32
							Sleep(250);
#else
							usleep(250000);
#endif
						}
					}
					continue;
				}
			}
			else
			{
				responseMatched = true;
			}
		}
		else
		{
			responseMatched = true;
		}

		int res;
		char buffer[MAXBUF];
		std::string pending;
		bool connectionLost = false;

		// Now process DX records
		while (m_started && responseMatched)
		{
			bzero(buffer, MAXBUF);
			res = recv(s, buffer, sizeof(buffer) - 1, 0);

			if (res == 0)
			{
				// Orderly shutdown by the server.
				Log("worker: server closed connection (recv=0)");
				connectionLost = true;
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
				Log("worker: recv error, reconnecting");
				connectionLost = true;
				break;
			}

			if (!m_started)
			{
				break;
			}

			buffer[res] = 0;
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
				Log("recv: " + line);
				if (line.find("DX de") != std::string::npos)
				{
					processDX(line.c_str());
				}
			}
			if (pending.size() > 65536)
			{
				// Never saw a newline; drop garbage rather than grow forever.
				pending.clear();
			}
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
#else
		close(s);
#endif

		if (!m_started || !connectionLost)
		{
			// User stopped, or the loop exited for a non-network reason.
			break;
		}

		Log("worker: connection lost, reconnecting in 5s");
		for (int i = 0; i < 20 && m_started; i++)
		{
#ifdef _WIN32
			Sleep(250);
#else
			usleep(250000);
#endif
		}
	}

#ifdef _WIN32
	WSACleanup();
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
	// Sanitise control characters (some clusters append BEL 0x07 after the
	// time field). Replace them with a space so they split off cleanly
	// instead of gluing onto the HHMMZ token and breaking the match.
	for (size_t i = 0; i < rest.size(); i++)
	{
		unsigned char c = (unsigned char)rest[i];
		if (c < 0x20)
		{
			rest[i] = ' ';
		}
	}
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
		Log("parse-drop: '" + line.substr(0, 120) + "'");
#ifdef DEBUG
		OutputDebugStringA("SDRuno process line error");
#endif
		return;
	}

	Log("accept: " + scsign + " " + std::to_string(llfreqHz) + "Hz " + stimeZ + "Z");
	long long newBand = BandOf(llfreqHz);
	{
		std::lock_guard<std::mutex> l(m_lock);
		if (head != NULL)
		{
			// Prune expired entries and older spots of the SAME callsign on
			// the SAME band (a DX station working several bands keeps one
			// marker per band). Unlink AND delete; keep prev on the
			// surviving chain (old code advanced prev onto the removed
			// node and leaked it).
			DXEntry* p = head;
			DXEntry* prevEntry = NULL;
			while (p != NULL)
			{
				DXEntry* next = p->next;
				bool sameCallsignSameBand =
					scsign.compare(p->callsign) == 0 &&
					BandOf(p->freq) == newBand;
				if (sameCallsignSameBand || olderThanTimer(p->timeUTC))
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

long long SDRunoPlugin_DXCluster::BandOf(long long freqHz)
{
	// Canonical anchor (Hz) for the amateur band a frequency belongs to.
	// Anything outside these ranges falls back to a 1 MHz-aligned bucket so
	// dedup still groups sensibly (e.g. broadcast/beacon spots).
	if (freqHz >= 1800000LL && freqHz < 2000000LL) return 1800000LL;      // 160m
	if (freqHz >= 3500000LL && freqHz < 4000000LL) return 3500000LL;      // 80m
	if (freqHz >= 5300000LL && freqHz < 5410000LL) return 5300000LL;      // 60m
	if (freqHz >= 7000000LL && freqHz < 7300000LL) return 7000000LL;      // 40m
	if (freqHz >= 10100000LL && freqHz < 10160000LL) return 10100000LL;   // 30m
	if (freqHz >= 14000000LL && freqHz < 14360000LL) return 14000000LL;   // 20m
	if (freqHz >= 18068000LL && freqHz < 18170000LL) return 18068000LL;   // 17m
	if (freqHz >= 21000000LL && freqHz < 21460000LL) return 21000000LL;   // 15m
	if (freqHz >= 24890000LL && freqHz < 24990000LL) return 24890000LL;   // 12m
	if (freqHz >= 28000000LL && freqHz < 29710000LL) return 28000000LL;   // 10m
	if (freqHz >= 50000000LL && freqHz < 54000000LL) return 50000000LL;   // 6m
	if (freqHz >= 70000000LL && freqHz < 71000000LL) return 70000000LL;   // 4m (EU)
	if (freqHz >= 144000000LL && freqHz < 148000000LL) return 144000000LL; // 2m
	if (freqHz >= 222000000LL && freqHz < 226000000LL) return 222000000LL; // 1.25m
	if (freqHz >= 430000000LL && freqHz < 450000000LL) return 430000000LL; // 70cm
	if (freqHz >= 902000000LL && freqHz < 928000000LL) return 902000000LL; // 33cm
	if (freqHz >= 1240000000LL && freqHz < 1300000000LL) return 1240000000LL; // 23cm
	if (freqHz >= 2300000000LL && freqHz < 2450000000LL) return 2300000000LL; // 13cm
	if (freqHz >= 3300000000LL && freqHz < 3500000000LL) return 3300000000LL; // 9cm
	if (freqHz >= 5650000000LL && freqHz < 5926000000LL) return 5650000000LL; // 6cm
	if (freqHz >= 10000000000LL && freqHz < 10501000000LL) return 10000000000LL; // 3cm
	if (freqHz >= 24000000000LL && freqHz < 24260000000LL) return 24000000000LL; // 1.2cm
	return (freqHz / 1000000LL) * 1000000LL;
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