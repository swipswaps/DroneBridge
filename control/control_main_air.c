/*
 *   This file is part of DroneBridge: https://github.com/seeul8er/DroneBridge
 *
 *   Copyright 2018 Wolfgang Christl
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <net/if.h>
#include <netinet/ether.h>
#include <termios.h>    // POSIX terminal control definitionss
#include <fcntl.h>      // File control definitions
#include <errno.h>      // Error number definitions
#include "../common/db_protocol.h"
#include <sys/time.h>
#include "../common/db_raw_send_receive.h"
#include "../common/db_raw_receive.h"
#include "rc_air.h"
#include "../common/mavlink/c_library_v2/common/mavlink.h"
#include "../common/msp_serial.h"
#include "../common/ccolors.h"
#include "../common/db_utils.h"
#include "../common/radiotap/radiotap_iter.h"


#define ETHER_TYPE        0x88ab
#define DEFAULT_IF      "wlx000ee8dcaa2c"
#define UART_IF          "/dev/serial1"
#define BUF_SIZ                        512 // should be enough?!
#define COMMAND_BUF_SIZE            1024
#define RETRANSMISSION_RATE 2  // send every MAVLink transparent packet twice for better reliability

static volatile int keepRunning = 1;
uint8_t buf[BUF_SIZ];
uint8_t mavlink_telemetry_buf[2048] = {0}, mavlink_message_buf[256] = {0};
int mav_tel_message_counter = 0, mav_tel_buf_length = 0, cont_adhere_80211, num_inf = 0;
long double cpu_u_new[4], cpu_u_old[4], loadavg;
float systemp, millideg;

void intHandler(int dummy) {
    keepRunning = 0;
}

speed_t interpret_baud(int user_baud) {
    switch (user_baud) {
        case 2400:
            return B2400;
        case 4800:
            return B4800;
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        default:
            return B115200;
    }
}


/**
 * Buffer 5 MAVLink telemetry messages before sending packet to ground station
 * @param length_message Length of new MAVLink message
 * @param mav_message The pointer to a new MAVLink message
 * @param proxy_seq_number
 * @param raw_interfaces_telem
 */
void send_buffered_mavlink(int length_message, mavlink_message_t *mav_message, uint8_t *proxy_seq_number,
                           db_socket_t *raw_interfaces_telem) {
    mav_tel_message_counter++;  // Number of messages in buffer
    mavlink_msg_to_send_buffer(mavlink_message_buf, mav_message);   // Get over the wire representation of message
    // Copy message bytes into buffer
    memcpy(&mavlink_telemetry_buf[mav_tel_buf_length], mavlink_message_buf, (size_t) length_message);
    mav_tel_buf_length += length_message;   // Overall length of buffer
    if (mav_tel_message_counter == 5) {
        for (int i = 0; i < num_inf; i++) {
            send_packet_div(&raw_interfaces_telem[i], mavlink_telemetry_buf, DB_PORT_PROXY,
                            (u_int16_t) mav_tel_buf_length, update_seq_num(proxy_seq_number),
                            cont_adhere_80211);
        }
        mav_tel_message_counter = 0;
        mav_tel_buf_length = 0;
    }
}

/**
 * Gets CPU usage on Linux systems. Needs to be called periodically. No one time calls!
 * @return CPU load in %
 */
uint8_t get_cpu_usage() {
    FILE *fp;
    fp = fopen("/proc/stat", "r");
    if (fscanf(fp, "%*s %Lf %Lf %Lf %Lf", &cpu_u_new[0], &cpu_u_new[1], &cpu_u_new[2], &cpu_u_new[3]) < 4)
        perror("DB_CONTROL_AIR: Could not read CPU usage\n");
    fclose(fp);
    loadavg = ((cpu_u_old[0] + cpu_u_old[1] + cpu_u_old[2]) - (cpu_u_new[0] + cpu_u_new[1] + cpu_u_new[2])) /
              ((cpu_u_old[0] + cpu_u_old[1] + cpu_u_old[2] + cpu_u_old[3]) -
               (cpu_u_new[0] + cpu_u_new[1] + cpu_u_new[2] + cpu_u_new[3])) * 100;
    memcpy(cpu_u_old, cpu_u_new, sizeof(cpu_u_new));
    return (uint8_t) loadavg;
}

/**
 * Reads the CPU temperature from a Linux system
 * @return CPU temperature in °C
 */
uint8_t get_cpu_temp() {
    FILE *thermal;
    thermal = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (fscanf(thermal, "%f", &millideg) < 1)
        perror("DB_CONTROL_AIR: Could not read CPU temperature\n");
    fclose(thermal);
    systemp = millideg / 1000;
    return (uint8_t) systemp;
}

int8_t get_rssi(uint8_t *payload_buffer, int radiotap_length) {
    struct ieee80211_radiotap_iterator rti;
    if (ieee80211_radiotap_iterator_init(&rti, (struct ieee80211_radiotap_header *) payload_buffer, radiotap_length,
                                         NULL) < 0)
        return 0;
    while ((ieee80211_radiotap_iterator_next(&rti)) == 0) {
        switch (rti.this_arg_index) {
            case IEEE80211_RADIOTAP_DBM_ANTSIGNAL:
                return (int8_t) (*rti.this_arg);
            default:
                break;
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    int c, bitrate_op = 1, chucksize = 64;
    int serial_protocol_control = 2, baud_rate = 115200;
    char use_sumd = 'N';
    char sumd_interface[IFNAMSIZ];
    char telem_inf[IFNAMSIZ];
    uint8_t comm_id = DEFAULT_V2_COMMID, frame_type = DB_FRAMETYPE_DEFAULT;
    uint8_t status_seq_number = 0, proxy_seq_number = 0, serial_byte;
    char db_mode = 'm';
    char adapters[DB_MAX_ADAPTERS][IFNAMSIZ];

// -------------------------------
// Processing command line arguments
// -------------------------------
    strcpy(telem_inf, UART_IF);
    strcpy(sumd_interface, UART_IF);
    cont_adhere_80211 = 0;
    opterr = 0;
    while ((c = getopt(argc, argv, "n:u:m:c:b:v:l:e:s:r:t:a:")) != -1) {
        switch (c) {
            case 'n':
                if (num_inf < DB_MAX_ADAPTERS) {
                    strncpy(adapters[num_inf], optarg, IFNAMSIZ);
                    num_inf++;
                }
                break;
            case 'u':
                strcpy(telem_inf, optarg);
                break;
            case 'm':
                db_mode = *optarg;
                break;
            case 'c':
                comm_id = (uint8_t) strtol(optarg, NULL, 10);
                break;
            case 'v':
                serial_protocol_control = (int) strtol(optarg, NULL, 10);
                break;
            case 'l':
                chucksize = (int) strtol(optarg, NULL, 10);
                break;
            case 'e':
                use_sumd = *optarg;
                break;
            case 's':
                strcpy(sumd_interface, optarg);
                break;
            case 'b':
                bitrate_op = (int) strtol(optarg, NULL, 10);
                break;
            case 'r':
                baud_rate = (int) strtol(optarg, NULL, 10);
                break;
            case 't':
                frame_type = (uint8_t) strtol(optarg, NULL, 10);
                break;
            case 'a':
                cont_adhere_80211 = (int) strtol(optarg, NULL, 10);
            case '?':
                printf("Invalid commandline arguments. Use "
                       "\n\t-n <Network interface name - multiple <-n interface> possible> "
                       "\n\t-u <MSP/MAVLink_Interface_TO_FC> - UART or VCP interface that is connected to FC"
                       "\n\t-m [w|m] (m = default, w = unsupported) DroneBridge mode - wifi/monitor"
                       "\n\t-v Protocol over serial port [1|2|3|4]:\n"
                       "\t\t1 = MSPv1 [Betaflight/Cleanflight]\n"
                       "\t\t2 = MSPv2 [iNAV] (default)\n"
                       "\t\t3 = MAVLink v1 (RC unsupported)\n"
                       "\t\t4 = MAVLink v2\n"
                       "\t\t5 = MAVLink (plain) pass through (-l <chunk size>) - recommended with MAVLink, "
                       "FC needs to support MAVLink v2 for RC"
                       "\n\t-l only relevant with -v 5 option. Telemetry bytes per packet over long range "
                       "(default: %i)"
                       "\n\t-e [Y|N] enable/disable RC over SUMD. If disabled -v & -u options are used for RC."
                       "\n\t-s Specify a serial port for use with SUMD. Ignored if SUMD is deactivated. Must be "
                       "different from one specified with -u"
                       "\n\t-c <communication_id> Choose a number from 0-255. Same on groundstation and drone!"
                       "\n\t-r Baud rate of the serial interface -u (MSP/MAVLink) (2400, 4800, 9600, 19200, "
                       "38400, 57600, 115200 (default: %i))"
                       "\n\t-t <1|2> DroneBridge v2 raw protocol packet/frame type: 1=RTS, 2=DATA (CTS protection)\n"
                       "\n\t-b bit rate:\tin Mbps (1|2|5|6|9|11|12|18|24|36|48|54)\n\t\t(bitrate option only "
                       "supported with Ralink chipsets)"
                       "\n\t-a <0|1> to disable/enable. Offsets the payload by some bytes so that it sits outside "
                       "then 802.11 header. Set this to 1 if you are using a non DB-Rasp Kernel!",
                       chucksize, baud_rate);
                break;
            default:
                abort();
        }
    }
    conf_rc_serial_protocol_air(serial_protocol_control, use_sumd);
    open_rc_rx_shm(); // open/init shared memory to write RC values into it
// -------------------------------
// Setting up network interface
// -------------------------------
    db_socket_t raw_interfaces_rc[DB_MAX_ADAPTERS] = {0};
    db_socket_t raw_interfaces_telem[DB_MAX_ADAPTERS] = {0};
    for (int i = 0; i < num_inf; ++i) {
        raw_interfaces_rc[i] = open_db_socket(adapters[i], comm_id, db_mode, bitrate_op, DB_DIREC_GROUND, DB_PORT_RC,
                                              frame_type);
        raw_interfaces_telem[i] = open_db_socket(adapters[i], comm_id, db_mode, bitrate_op, DB_DIREC_GROUND,
                                                 DB_PORT_CONTROLLER, frame_type);
    }

// -------------------------------
// Setting up UART interface for MSP/MAVLink stream
// -------------------------------
    uint8_t transparent_buffer[chucksize];
    int socket_control_serial = -1;
    do {
        socket_control_serial = open(telem_inf, O_RDWR | O_NOCTTY | O_SYNC);
        if (socket_control_serial == -1) {
            printf(YEL"DB_CONTROL_AIR: Error - Unable to open UART for MSP/MAVLink.  Ensure it is not in use by another "
                   "application and the FC is connected\n");
            printf("DB_CONTROL_AIR: retrying ..."RESET"\n");
            sleep(1);
        }
    } while (socket_control_serial == -1);

    struct termios options;
    tcgetattr(socket_control_serial, &options);
    options.c_iflag &= ~(IGNBRK | BRKINT | ICRNL | INLCR | PARMRK | INPCK | ISTRIP | IXON);
    options.c_oflag &= ~(OCRNL | ONLCR | ONLRET | ONOCR | OFILL | OPOST);
    options.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
    options.c_cflag &= ~(CSIZE | PARENB);
    options.c_cflag |= CS8;
    cfsetispeed(&options, interpret_baud(baud_rate));
    cfsetospeed(&options, interpret_baud(baud_rate));

    options.c_cc[VMIN] = 1;            // wait for min. 1 byte (select trigger)
    options.c_cc[VTIME] = 0;           // timeout 0 second
    tcflush(socket_control_serial, TCIFLUSH);
    tcsetattr(socket_control_serial, TCSANOW, &options);
    int rc_serial_socket = socket_control_serial;

// -------------------------------
// Setting up UART interface for RC commands over SUMD
// -------------------------------
    int socket_rc_serial = -1;
    if (use_sumd == 'Y') {
        do {
            socket_rc_serial = open(sumd_interface, O_WRONLY | O_NOCTTY | O_SYNC);
            if (socket_rc_serial == -1) {
                printf(RED "DB_CONTROL_AIR: Error - Unable to open UART for SUMD RC.  Ensure it is not in use by another"
                       " application and the FC is connected. Retrying ... "RESET"\n");
                sleep(1);
            }
        } while (socket_rc_serial == -1);

        struct termios options_rc;
        tcgetattr(socket_rc_serial, &options_rc);
        cfsetospeed(&options_rc, B115200);
        options_rc.c_iflag &= ~(IGNBRK | BRKINT | ICRNL | INLCR | PARMRK | INPCK | ISTRIP | IXON);
        options_rc.c_oflag &= ~(OCRNL | ONLCR | ONLRET | ONOCR | OFILL | OPOST);
        options_rc.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
        options_rc.c_cflag &= ~(CSIZE | PARENB);
        options_rc.c_cflag |= CS8;
        tcflush(socket_rc_serial, TCIFLUSH);
        tcsetattr(socket_rc_serial, TCSANOW, &options_rc);
        rc_serial_socket = socket_rc_serial;
    }

// ----------------------------------
// Loop
// ----------------------------------
    int sentbytes = 0, command_length = 0, errsv, select_return, continue_reading, chunck_left = chucksize,
    serial_read_bytes = 0, max_sd = 0;
    int8_t rssi = 0, last_recv_rc_seq_num = 0, last_recv_cont_seq_num = 0;
    long start; // start time for status report update
    long start_rc; // start time for measuring the recv RC packets/second
    long rightnow, status_report_update_rate = 200; // send rc status to status module on groundstation every 200ms

    uint8_t rc_packets_tmp = 0, rc_packets_cnt = 0, seq_num_rc = 0, seq_num_cont = 0;
    mavlink_message_t mavlink_message;
    mavlink_status_t mavlink_status;
    mspPort_t db_msp_port;

    fd_set fd_socket_set;
    struct timeval socket_timeout;
    socket_timeout.tv_sec = 0;
    socket_timeout.tv_usec =
            status_report_update_rate * 1000; // wait max status_report_update_rate for message on socket

    ssize_t length;
    signal(SIGINT, intHandler);
    uint8_t commandBuf[COMMAND_BUF_SIZE];
    struct timeval timecheck;

    // create our data pointer directly inside the buffer (monitor_framebuffer) that is sent over the socket
    struct data_uni *raw_buffer = get_hp_raw_buffer(cont_adhere_80211);
    struct uav_rc_status_update_message_t *rc_status_update_data = (struct uav_rc_status_update_message_t *) raw_buffer;
    memset(raw_buffer->bytes, 0, DATA_UNI_LENGTH);

    printf(GRN "DB_CONTROL_AIR: Ready for data! Enabled diversity on %i adapters"RESET"\n", num_inf);
    gettimeofday(&timecheck, NULL);
    start = (long) timecheck.tv_sec * 1000 + (long) timecheck.tv_usec / 1000;
    start_rc = start;
    uint16_t radtap_lenght;
    while (keepRunning) {
        socket_timeout.tv_sec = 0;
        socket_timeout.tv_usec = status_report_update_rate * 1000;
        FD_ZERO (&fd_socket_set);
        // add raw DroneBridge sockets
        for (int i = 0; i < num_inf; i++) {
            FD_SET (raw_interfaces_rc[i].db_socket, &fd_socket_set);
            if (raw_interfaces_rc[i].db_socket > max_sd)
                max_sd = raw_interfaces_rc[i].db_socket;
            if (raw_interfaces_telem[i].db_socket > max_sd)
                max_sd = raw_interfaces_telem[i].db_socket;
        }
        FD_SET (socket_control_serial, &fd_socket_set);
        if (socket_control_serial > max_sd)
            max_sd = socket_control_serial;
        select_return = select(max_sd + 1, &fd_socket_set, NULL, NULL, &socket_timeout);

        if (select_return == -1) {
            perror("DB_CONTROL_AIR: select() returned error: ");
        } else if (select_return > 0) {
            // --------------------------------
            // DroneBridge long range data
            // --------------------------------
            for (int i = 0; i < num_inf; i++) {
                if (FD_ISSET(raw_interfaces_rc[i].db_socket, &fd_socket_set)) {
                    // --------------------------------
                    // DB_RC_PORT for DroneBridge RC packets
                    // --------------------------------
                    length = recv(raw_interfaces_rc[i].db_socket, buf, BUF_SIZ, 0);
                    if (length > 0) {
                        rssi = get_rssi(buf, buf[2]);
                        get_db_payload(buf, length, commandBuf, &seq_num_rc, &radtap_lenght);
                        if (last_recv_rc_seq_num != seq_num_rc) {  // diversity duplicate protection
                            last_recv_rc_seq_num = seq_num_rc;
                            command_length = generate_rc_serial_message(commandBuf);
                            if (command_length > 0) {
                                rc_packets_cnt++;
                                sentbytes = (int) write(rc_serial_socket, serial_data_buffer, (size_t) command_length);
                                errsv = errno;
                                tcdrain(rc_serial_socket);
                                if (sentbytes <= 0) {
                                    printf(RED "RC NOT WRITTEN because of error: %s"RESET"\n", strerror(errsv));
                                }
                                // TODO: check if necessary. It shouldn't as we use blocking UART socket
                                // tcflush(rc_serial_socket, TCOFLUSH);
                            }
                        }
                    }
                }
            }

            for (int i = 0; i < num_inf; i++) {
                if (FD_ISSET(raw_interfaces_telem[i].db_socket, &fd_socket_set)) {
                    // --------------------------------
                    // DB_CONTROL_PORT for MSP/MAVLink
                    // --------------------------------
                    length = recv(raw_interfaces_telem[i].db_socket, buf, BUF_SIZ, 0);
                    if (length > 0) {
                        rssi = get_rssi(buf, buf[2]);
                        if (last_recv_cont_seq_num != seq_num_cont) {  // diversity duplicate protection
                            last_recv_cont_seq_num = seq_num_cont;
                            command_length = get_db_payload(buf, length, commandBuf, &seq_num_cont, &radtap_lenght);
                            sentbytes = (int) write(socket_control_serial, commandBuf, (size_t) command_length);
                            errsv = errno;
                            tcdrain(socket_control_serial);
                            if (sentbytes < command_length) {
                                printf(RED"MSP/MAVLink NOT WRITTEN because of error: %s"RESET"\n", strerror(errsv));
                            }
                            // TODO: check if necessary. It shouldn't as we use blocking UART socket
                            // tcflush(socket_control_serial, TCOFLUSH);
                        }
                    }
                }
            }
            // --------------------------------
            // FC input
            // --------------------------------
            if (FD_ISSET(socket_control_serial, &fd_socket_set)) {
                // --------------------------------
                // The FC sent us a MSP/MAVLink message - LTM telemetry will be ignored!
                // --------------------------------
                switch (serial_protocol_control) {
                    default:
                    case 1:
                    case 2:
                        // Parse MSP message - just pass it to DB proxy module on ground station
                        continue_reading = 1;
                        serial_read_bytes = 0;
                        while (continue_reading) {
                            if (read(socket_control_serial, &serial_byte, 1) > 0) {
                                serial_read_bytes++;
                                // if MSP parser returns false stop reading from serial. We are reading shit or started
                                // reading during the middle of a message
                                if (mspSerialProcessReceivedData(&db_msp_port, serial_byte)) {
                                    raw_buffer->bytes[(serial_read_bytes - 1)] = serial_byte;
                                    if (db_msp_port.c_state == MSP_COMMAND_RECEIVED) {
                                        continue_reading = 0; // stop reading from serial port --> got a complete message!
                                        for (int i = 0; i < num_inf; i++) {
                                            send_packet_hp_div(&raw_interfaces_telem[i], DB_PORT_PROXY,
                                                               (u_int16_t) serial_read_bytes,
                                                               update_seq_num(&proxy_seq_number));
                                        }
                                    }
                                } else {
                                    continue_reading = 0;
                                }
                            }
                        }
                        break;
                    case 3:
                    case 4:
                        // Parse complete MAVLink message
                        continue_reading = 1;
                        serial_read_bytes = 0;
                        while (continue_reading) {
                            if (read(socket_control_serial, &serial_byte, 1) > 0) {
                                serial_read_bytes++;
                                if (mavlink_parse_char(MAVLINK_COMM_0, (uint8_t) serial_byte, &mavlink_message,
                                                       &mavlink_status)) {
                                    continue_reading = 0; // stop reading from serial port --> got a complete message!
                                    mavlink_msg_to_send_buffer(raw_buffer->bytes, &mavlink_message);
                                    for (int i = 0; i < num_inf; i++) {
                                        send_packet_hp_div(&raw_interfaces_telem[i], DB_PORT_PROXY,
                                                        (u_int16_t) chucksize, update_seq_num(&proxy_seq_number));
                                    }
                                }
                            }
                        }
                        break;
                    case 5:
                        // MAVLink plain pass through - no parsing. Send packets with length of chuck size
                        if (read(socket_control_serial, &serial_byte, 1) > 0) {
                            transparent_buffer[serial_read_bytes] = serial_byte;
                            serial_read_bytes++;
                            if (serial_read_bytes == chucksize) {
                                for (int i = 0; i < num_inf; i++) {
                                    for (int r = 0; r < RETRANSMISSION_RATE; r ++)
                                        send_packet_div(&raw_interfaces_telem[i], transparent_buffer, DB_PORT_PROXY,
                                                (u_int16_t) chucksize, update_seq_num(&proxy_seq_number),
                                                cont_adhere_80211);
                                }
                                serial_read_bytes = 0;
                            }
                        }
                        break;
                }
            }
        }

        // --------------------------------
        // Send a status update to status module on ground station
        // --------------------------------
        gettimeofday(&timecheck, NULL);
        rightnow = (long) timecheck.tv_sec * 1000 + (long) timecheck.tv_usec / 1000;
        if (rightnow - start_rc >= 1000) {
            rc_packets_tmp = rc_packets_cnt; // save received packets/seconds to temp variable
            rc_packets_cnt = 0;
            start_rc = (long) timecheck.tv_sec * 1000 + (long) timecheck.tv_usec / 1000;
        }
        if ((rightnow - start) >= status_report_update_rate) {
            memset(rc_status_update_data, 0xff, 6);
            rc_status_update_data->rssi_rc_uav = rssi;
            // lost packets/second (it is a estimate)
            rc_status_update_data->recv_pack_sec = rc_packets_tmp;
            rc_status_update_data->cpu_usage_uav = get_cpu_usage();
            rc_status_update_data->cpu_temp_uav = get_cpu_temp();
            rc_status_update_data->uav_is_low_V = get_undervolt();
            for (int i = 0; i < num_inf; i++) {
                send_packet_hp_div(&raw_interfaces_telem[i], DB_PORT_STATUS,
                                   (u_int16_t) 6, update_seq_num(&status_seq_number));
            }

            gettimeofday(&timecheck, NULL);
            start = (long) timecheck.tv_sec * 1000 + (long) timecheck.tv_usec / 1000;
        }
    }
    for (int i = 0; i < DB_MAX_ADAPTERS; i++) {
        if (raw_interfaces_rc[i].db_socket > 0)
            close(raw_interfaces_rc[i].db_socket);
        if (raw_interfaces_telem[i].db_socket > 0)
            close(raw_interfaces_telem[i].db_socket);
    }
    close(socket_control_serial);
    close(rc_serial_socket);
    printf("DB_CONTROL_AIR: Sockets closed!\n");
    return 1;
}
