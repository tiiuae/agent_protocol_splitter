/****************************************************************************
 *
 * Copyright (c) 2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include <protocol_splitter.hpp>

#include <atomic>
#include <thread>
#include <mutex>

std::mutex mtx;

int ReadBuffer::read(int fd)
{
	/* Discard whole buffer if it's filled beyond a threshold,
	 * This should prevent buffer being filled by garbage that
	 * no reader (MAVLink or RTPS) can understand.
	 */
	if (buf_size > BUFFER_THRESHOLD) {
		buf_size = 0;
	}

	int r = ::read(fd, buffer + buf_size, sizeof(buffer) - buf_size);

	if (r < 0) {
		return r;
	}

	buf_size += r;

	return r;
}

void ReadBuffer::move(void *dest, size_t pos, size_t n)
{
	assert(pos < buf_size);
	assert(pos + n <= buf_size);

	memmove(dest, buffer + pos, n); // send desired data
	memmove(buffer + pos, buffer + (pos + n), sizeof(buffer) - pos - n);
	buf_size -= n;
}

DevSerial::DevSerial(const char *device_name, const uint32_t baudrate, const bool hw_flow_control,
		     const bool sw_flow_control)
	: _baudrate(baudrate),
	  _hw_flow_control(hw_flow_control),
	  _sw_flow_control(sw_flow_control)
{
	strncpy(_uart_name, device_name, sizeof(_uart_name));
}

DevSerial::~DevSerial()
{
	if (_uart_fd >= 0) {
		close();
	}
}

int DevSerial::open_uart()
{
	// Open a serial port, if not opened already
	if (_uart_fd < 0) {
		_uart_fd = open(_uart_name, O_RDWR | O_NOCTTY | O_NONBLOCK);

		if (_uart_fd < 0) {
			printf("\033[0;31m[ protocol__splitter ]\tSerial link: Failed to open device: %s (%d)\033[0m\n", _uart_name, errno);
			return -errno;
		}

		// If using shared UART, no need to set it up
		if (_baudrate == 0) {
			return _uart_fd;
		}

		// Try to set baud rate
		struct termios uart_config;
		int termios_state;

		// Back up the original uart configuration to restore it after exit
		if ((termios_state = tcgetattr(_uart_fd, &uart_config)) < 0) {
			int errno_bkp = errno;
			printf("\033[0;31m[ protocol__splitter ]\tSerial link: Error getting config %s: %d (%d)\033[0m\n", _uart_name,
			       termios_state, errno);
			close();
			return -errno_bkp;
		}

		// Set up the UART for non-canonical binary communication: 8 bits, 1 stop bit, no parity.
		uart_config.c_iflag &= !(INPCK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXANY | IXOFF);
		uart_config.c_iflag |= IGNBRK | IGNPAR;

		uart_config.c_oflag &= !(OPOST | ONLCR | OCRNL | ONOCR | ONLRET | OFILL | NLDLY | VTDLY);
		uart_config.c_oflag |= NL0 | VT0;

		uart_config.c_cflag &= !(CSIZE | CSTOPB | PARENB);
		uart_config.c_cflag |= CS8 | CREAD | CLOCAL;

		uart_config.c_lflag &= !(ISIG | ICANON | ECHO | TOSTOP | IEXTEN);

		// Flow control
		if (_hw_flow_control) {
			// HW flow control
			uart_config.c_lflag |= CRTSCTS;

		} else if (_sw_flow_control) {
			// SW flow control
			uart_config.c_lflag |= (IXON | IXOFF | IXANY);
		}

		// Set baud rate
		speed_t speed;

		if (!baudrate_to_speed(_baudrate, &speed)) {
			printf("\033[0;31m[ protocol__splitter ]\tSerial link: Error setting baudrate %s: Unsupported baudrate: %d\n\tsupported examples:\n\t9600, 19200, 38400, 57600, 115200, 230400, 460800, 500000, 921600, 1000000\033[0m\n",
			       _uart_name, _baudrate);
			close();
			return -EINVAL;
		}

		if (cfsetispeed(&uart_config, speed) < 0 || cfsetospeed(&uart_config, speed) < 0) {
			int errno_bkp = errno;
			printf("\033[0;31m[ protocol__splitter ]\tSerial link: Error setting baudrate %s: %d (%d)\033[0m\n", _uart_name,
			       termios_state, errno);
			close();
			return -errno_bkp;
		}

		if ((termios_state = tcsetattr(_uart_fd, TCSANOW, &uart_config)) < 0) {
			int errno_bkp = errno;
			printf("\033[0;31m[ protocol__splitter ]\tSerial link: ERR SET CONF %s (%d)\033[0m\n", _uart_name, errno);
			close();
			return -errno_bkp;
		}

		printf("[ protocol__splitter ]\tSerial link: device: %s; baudrate: %d; flow_control: %s\n",
		       _uart_name, _baudrate, _sw_flow_control ? "SW enabled" : (_hw_flow_control ? "HW enabled" : "No"));

		char aux[64];
		bool flush = false;

		while (0 < ::read(_uart_fd, (void *)&aux, 64)) {
			flush = true;
			usleep(1000);
		}

		if (flush) {
			printf("[ protocol__splitter ]\tSerial link: Flush\n");

		} else {
			printf("[ protocol__splitter ]\tSerial link: No flush\n");
		}
	}

	return _uart_fd;
}

bool DevSerial::baudrate_to_speed(uint32_t bauds, speed_t *speed)
{
#ifndef B460800
#define B460800 460800
#endif

#ifndef B500000
#define B500000 500000
#endif

#ifndef B921600
#define B921600 921600
#endif

#ifndef B1000000
#define B1000000 1000000
#endif

#ifndef B1500000
#define B1500000 1500000
#endif

#ifndef B2000000
#define B2000000 2000000
#endif

	switch (bauds) {
	case 0:      *speed = B0;		break;

	case 50:     *speed = B50;		break;

	case 75:     *speed = B75;		break;

	case 110:    *speed = B110;		break;

	case 134:    *speed = B134;		break;

	case 150:    *speed = B150;		break;

	case 200:    *speed = B200;		break;

	case 300:    *speed = B300;		break;

	case 600:    *speed = B600;		break;

	case 1200:   *speed = B1200;		break;

	case 1800:   *speed = B1800;		break;

	case 2400:   *speed = B2400;		break;

	case 4800:   *speed = B4800;		break;

	case 9600:   *speed = B9600;		break;

	case 19200:  *speed = B19200;		break;

	case 38400:  *speed = B38400;		break;

	case 57600:  *speed = B57600;		break;

	case 115200: *speed = B115200;		break;

	case 230400: *speed = B230400;		break;

	case 460800: *speed = B460800;		break;

	case 500000: *speed = B500000;		break;

	case 921600: *speed = B921600;		break;

	case 1000000: *speed = B1000000;	break;

	case 1500000: *speed = B1500000;	break;

	case 2000000: *speed = B2000000;	break;

#ifdef B3000000

	case 3000000: *speed = B3000000;    break;
#endif

#ifdef B3500000

	case 3500000: *speed = B3500000;    break;
#endif

#ifdef B4000000

	case 4000000: *speed = B4000000;    break;
#endif

	default:
		return false;
	}

	return true;
}

int DevSerial::close()
{
	if (_uart_fd >= 0) {
		printf("\033[1;33m[ protocol__splitter ]\tSerial link: Closed serial port!\033[0m\n");
		::close(_uart_fd);
		_uart_fd = -1;
	}

	return 0;
}

DevSocket::DevSocket(const char *udp_ip, const uint16_t udp_port_recv,
		     const uint16_t udp_port_send, int uart_fd)
	: _uart_fd(uart_fd)
	, _udp_fd(-1)
	, _udp_port_recv(udp_port_recv)
	, _udp_port_send(udp_port_send)
{
	if (nullptr != udp_ip) {
		strcpy(_udp_ip, udp_ip);
	}

	open_udp();
}

DevSocket::~DevSocket()
{
	// Close the sender
	if (_udp_fd >= 0) {
		close(_udp_fd);
	}

	// Close the receiver
	if (_udp_fd >= 0) {
		close(_udp_fd);
	}
}

int DevSocket::open_udp()
{
	// Init receiver
	if ((_udp_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		printf("\033[0;31m[ protocol__splitter ]\tUDP socket link: Create socket failed\033[0m\n");
		return -1;
	}

	memset((char *)&_inaddr, 0, sizeof(_inaddr));
	_inaddr.sin_family = AF_INET;
	_inaddr.sin_port = htons(_udp_port_recv);
	_inaddr.sin_addr.s_addr = htonl(INADDR_ANY);

	memset((char *) &_outaddr, 0, sizeof(_outaddr));
	_outaddr.sin_family = AF_INET;
	_outaddr.sin_port = htons(_udp_port_send);
	_outaddr.sin_addr.s_addr = htonl(INADDR_ANY);

	printf("[ protocol__splitter ]\tUDP socket link: Trying to connect...\n");

	if (bind(_udp_fd, (struct sockaddr *)&_inaddr, sizeof(_inaddr)) < 0) {
		printf("\033[0;31m[ protocol__splitter ]\tUDP socket link: Bind failed\033[0m\n");
		return -1;
	}

	printf("[ protocol__splitter ]\tUDP socket link: Connected to server!\n");

	if (inet_aton(_udp_ip, &_outaddr.sin_addr) == 0) {
		printf("\033[0;31m[ protocol__splitter ]\tUDP socket link: inet_aton() failed\033[0m\n");
		return -1;
	}

	return 0;
}

int DevSocket::close(int udp_fd)
{
	if (udp_fd >= 0) {
		printf("\033[1;33m[ protocol__splitter ]\tUDP socket link: Closed socket!\033[0m\n");
		shutdown(udp_fd, SHUT_RDWR);
		::close(udp_fd);
		udp_fd = -1;
	}

	return 0;
}

ssize_t DevSocket::udp_read(void *buffer, size_t len)
{
	if (nullptr == buffer || !(-1 != _udp_fd)) {
		return -1;
	}

	int ret = 0;
	static socklen_t addrlen = sizeof(_outaddr);
	if (ntohs(_outaddr.sin_port) == 0) {
		ret = recvfrom(_udp_fd, buffer, len, 0, (struct sockaddr *) &_outaddr, &addrlen);
	} else {
		ret = recv(_udp_fd, buffer, len, 0);
	}
	return ret;
}

ssize_t DevSocket::udp_write(void *buffer, size_t len)
{
	if (nullptr == buffer || !(-1 != _udp_fd)) {
		return -1;
	}

	int ret = 0;
	ret = sendto(_udp_fd, buffer, len, 0, (struct sockaddr *)&_outaddr, sizeof(_outaddr));
	return ret;
}

Mavlink2Dev::Mavlink2Dev(ReadBuffer *in_read_buffer, const char *udp_ip,
			 const uint16_t udp_port_recv,
			 const uint16_t udp_port_send,
			 int uart_fd)
	: DevSocket(udp_ip, udp_port_recv, udp_port_send, uart_fd)
	, _in_read_buffer{in_read_buffer}
{
}

ssize_t Mavlink2Dev::read()
{
	std::unique_lock<std::mutex> guard(mtx);

	int i = 0, ret = 0;
	uint16_t packet_len = 0;

	char buffer[BUFFER_SIZE];
	size_t buflen = sizeof(buffer);

	// Search for a mavlink packet on buffer to send it

	while (_in_read_buffer->buf_size >= 3) {
		while ((unsigned)i < (_in_read_buffer->buf_size - 3)
		       && _in_read_buffer->buffer[i] != 253
		       && _in_read_buffer->buffer[i] != 254) {
			i++;
		}

		// We need at least the first three bytes to get packet len
		if ((unsigned)i >= _in_read_buffer->buf_size - 3) {
			break;
		}

		if (_in_read_buffer->buffer[i] == 253) {
			uint8_t payload_len = _in_read_buffer->buffer[i + 1];
			uint8_t incompat_flags = _in_read_buffer->buffer[i + 2];
			packet_len = payload_len + 12;

			if (incompat_flags & 0x1) { //signing
				packet_len += 13;
			}

		} else {
			packet_len = _in_read_buffer->buffer[i + 1] + 8;
		}

		// packet is bigger than what we've read, better luck next time
		if ((unsigned)i + packet_len > _in_read_buffer->buf_size) {
			ret = -EMSGSIZE;
			break;
		}

		_in_read_buffer->move(buffer, i, packet_len);

		// Write to UDP port
		udp_write(buffer, packet_len);

		ret += packet_len;

	}

	guard.unlock();

	return ret;
}

ssize_t Mavlink2Dev::write()
{
	static char buffer[BUFFER_SIZE];
	static size_t buflen = sizeof(buffer);

	// Read from UDP port
	ssize_t ret = udp_read((void *)(buffer), buflen);

	if (ret < 0) {
		return ret;
	}

	if ((ret < 3) ||	// Check there is enough data for message
		((uint8_t)buffer[0] != 253 &&
		 (uint8_t)buffer[0] != 254)) // Check there is valid header byte
	{
		return 0;
	}

	std::unique_lock<std::mutex> guard(mtx);
	ret = ::write(_uart_fd, buffer, ret);
	guard.unlock();

	return ret;
}

RtpsDev::RtpsDev(ReadBuffer *in_read_buffer, const char *udp_ip,
		 const uint16_t udp_port_recv, const uint16_t udp_port_send,
		 int uart_fd)
	: DevSocket(udp_ip, udp_port_recv, udp_port_send, uart_fd)
	, _in_read_buffer{in_read_buffer}
{
}

ssize_t RtpsDev::read()
{
	std::unique_lock<std::mutex> guard(mtx);

	int i = 0, ret = 0;
	uint16_t packet_len, payload_len;

	char buffer[BUFFER_SIZE];
	size_t buflen = sizeof(buffer);

	// Search for a rtps packet on buffer to send it

	while (_in_read_buffer->buf_size >= HEADER_SIZE) {
		while ((unsigned)i < (_in_read_buffer->buf_size - HEADER_SIZE)
		       && (memcmp(_in_read_buffer->buffer + i, ">>>", 3) != 0)) {
			i++;
		}

		// We need at least the first six bytes to get packet len
		if ((unsigned)i > _in_read_buffer->buf_size - HEADER_SIZE) {
			ret = -1;
			break;
		}

		payload_len = ((uint16_t)_in_read_buffer->buffer[i + 5] << 8) | _in_read_buffer->buffer[i + 6];
		packet_len = payload_len + HEADER_SIZE;

		// packet is bigger than what we've read, better luck next time
		if ((unsigned)i + packet_len > _in_read_buffer->buf_size) {
			ret = -1;
			break;
		}

		_in_read_buffer->move(buffer, i, packet_len);

		// Write to UDP port
		udp_write(buffer, packet_len);

		ret += packet_len;

	}

	guard.unlock();

	return ret;
}

ssize_t RtpsDev::write()
{
	static char buffer[BUFFER_SIZE];
	static size_t buflen = sizeof(buffer);

	// Read from UDP port
	ssize_t ret = udp_read((void *)(buffer), buflen);

	if (ret < 0) {
		return ret;
	}

	if ((ret < 6) ||	// Check there is enough data for message
		(buffer[0] != '>' ||
		 buffer[1] != '>' ||
		 buffer[2] != '>') || // Check there is valid header
		( ((uint16_t)(buffer[5] << 8) | buffer[6])) > buflen ) // Check the message fits into buffer
	{
		return 0;
	}

	std::unique_lock<std::mutex> guard(mtx);
	ret = ::write(_uart_fd, buffer, ret);
	guard.unlock();

	return ret;
}

void signal_handler(int signum)
{
	printf("\033[1;33m[ protocol__splitter ]\tInterrupt signal (%d) received.\033[0m\n", signum);
	running = false;
}

void serial_to_udp(pollfd *fds)
{
	while (running) {
		if ((::poll(fds, sizeof(fds) / sizeof(fds[0]), 100) > 0) && (fds[0].revents & POLLIN)) {
			int len = objects->in_read_buffer->read(fds[0].fd);
			if (len) {
				objects->rtps->read();
				objects->mavlink2->read();
			}
		}
	}
}

void mavlink_udp_to_serial(pollfd *fds)
{
	while (running) {
		if ((::poll(fds, sizeof(fds) / sizeof(fds[0]), 100) > 0) && (fds[0].revents & POLLIN)) {
			objects->mavlink2->write();
		}
	}
}

void rtps_udp_to_serial(pollfd *fds)
{
	while (running) {
		if ((::poll(fds, sizeof(fds) / sizeof(fds[0]), 100) > 0) && (fds[0].revents & POLLIN)) {
			objects->rtps->write();
		}
	}
}

static void usage(const char *name)
{
	printf("usage: %s [options]\n\n"
	       "  -b <baudrate>			UART device baudrate. Default 460800\n"
	       "  -d <uart_device>		UART device. Default /dev/ttyUSB0\n"
	       "  -i <host_ip>			Host IP for UDP. Default 127.0.0.1\n"
	       "  -w <mavlink_udp_recv_port>	UDP port for receiving. Default 5800.\n"
	       "                            	 Set 0 to autoselect.\n"
	       "  -x <mavlink_udp_send_port>	UDP port for receiving. Default 5801.\n"
	       "                            	 Set 0 to get source port.\n"
	       "  -y <rtps_udp_recv_port>	UDP port for receiving. Default 5900.\n"
	       "                         	 Set 0 to autoselect.\n"
	       "  -z <rtps_udp_send_port>	UDP port for receiving. Default 5901\n"
	       "                         	 Set 0 to get source port.\n"
	       "  -f <sw_flow_control>		Activates UART link SW flow control\n"
	       "  -g <hw_flow_control>		Activates UART link HW flow control\n"
	       "  -v <verbose_debug>		Add more verbosity\n\n",
	       name);
}

static int parse_options(int argc, char **argv)
{
	int ch;

	while ((ch = getopt(argc, argv, "b:d:i:w:x:y:z:fghv")) != EOF) {
		switch (ch) {
		case 'b': _options.baudrate		  = strtoul(optarg, nullptr, 10);		  break;

		case 'd': if (nullptr != optarg)	strcpy(_options.uart_device, optarg); break;

		case 'i': if (nullptr != optarg)	strcpy(_options.host_ip, optarg);  	  break;

		case 'f': _options.sw_flow_control = true;								  break;

		case 'g': _options.hw_flow_control = true;								  break;

		case 'h': usage(argv[0]); return -1;									  break;

		case 'v': _options.verbose_debug = true;								  break;

		case 'w': _options.mavlink_udp_recv_port  = strtoul(optarg, nullptr, 10); break;

		case 'x': _options.mavlink_udp_send_port  = strtoul(optarg, nullptr, 10); break;

		case 'y': _options.rtps_udp_recv_port     = strtoul(optarg, nullptr, 10); break;

		case 'z': _options.rtps_udp_send_port     = strtoul(optarg, nullptr, 10); break;

		default:
			usage(argv[0]);
			return -1;
		}
	}

	if (_options.hw_flow_control && _options.sw_flow_control) {
		printf("\033[0;31m[ protocol__splitter ]\tHW and SW flow control set. Please set only one or another\033[0m\n");
		return -1;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	if (-1 == parse_options(argc, argv)) {
		return -1;
	}

	objects = new StaticData();

	std::signal(SIGINT, signal_handler);

	// Init the read buffer
	objects->in_read_buffer = new ReadBuffer();

	// Init the serial device
	objects->serial = new DevSerial(_options.uart_device, _options.baudrate, _options.hw_flow_control,
					_options.sw_flow_control);
	int uart_fd = objects->serial->open_uart();

	// Init UDP sockets for Mavlink and RTPS
	objects->mavlink2 = new Mavlink2Dev(objects->in_read_buffer,
					_options.host_ip, _options.mavlink_udp_recv_port, _options.mavlink_udp_send_port, uart_fd);
	objects->rtps = new RtpsDev(objects->in_read_buffer,
					_options.host_ip, _options.rtps_udp_recv_port, _options.rtps_udp_send_port, uart_fd);

	// Init fd polling
	pollfd fd_uart[1]{};
	pollfd fds_udp_mavlink[1]{};
	pollfd fds_udp_rtps[1]{};

	fd_uart[0].fd = uart_fd;
	fd_uart[0].events = POLLIN;

	fds_udp_mavlink[0].fd = objects->mavlink2->_udp_fd;
	fds_udp_mavlink[0].events = POLLIN;

	fds_udp_rtps[0].fd = objects->rtps->_udp_fd;
	fds_udp_rtps[0].events = POLLIN;

	running = true;

	std::thread serial_to_udp_th(serial_to_udp, fd_uart);
	std::thread rtps_udp_to_serial_th(rtps_udp_to_serial, fds_udp_rtps);
	std::thread mavlink_udp_to_serial_th(mavlink_udp_to_serial, fds_udp_mavlink);

	serial_to_udp_th.join();
	mavlink_udp_to_serial_th.join();
	rtps_udp_to_serial_th.join();

	delete objects->serial;
	delete objects->mavlink2;
	delete objects->rtps;
	delete objects->in_read_buffer;
	delete objects;
	objects = nullptr;

	printf("\033[1;33m[ protocol__splitter ]\tEXITING...\033[0m\n");

	return 0;
}
