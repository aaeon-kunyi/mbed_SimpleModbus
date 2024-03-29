#include "mbed.h"
#include "SimpleModbus.h"
// #define _DEBUG_MODBUS (1)
#ifdef _DEBUG_MODBUS
#include "mbed_trace.h"
#define TRACE_GROUP     "modbus"
#define MODBUS_DEBUG    tr_debug
#define DEBUGMSG_DUMP(str, size, buff)       \
                do { int i; \
                    printf("[%s:%s:%d]: "str"[%d]: ", TRACE_GROUP, __FUNCTION__, __LINE__, size); \
                    for(i = 0; i < size; i++) \
                        printf("%02x ", buff[i]); \
                    printf("\n"); } while (0)
                    
#define MODBUS_DUMP     DEBUGMSG_DUMP
#else
#define MODBUS_DEBUG
#define DEBUGMSG_DUMP(str, size, buff)
#define MODBUS_DUMP
#endif


// state machine states
#define IDLE                    1
#define WAITING_FOR_REPLY       2
#define WAITING_FOR_TURNAROUND  3
#define DONE                    4
#define INVALID                 5

#define BUFFER_SIZE 100

Timer ModBusTimer;
UARTSerial ModbusPort(MODBUS_TX, MODBUS_RX, 9600);


static unsigned char state;
static unsigned char retry_count;

static unsigned int packet_index;


// frame[] is used to receive and transmit packages.
// The maximum number of bytes in a modbus packet is 256 bytes
// This is limited to the serial buffer of 64 bytes
unsigned char frame[BUFFER_SIZE];
unsigned char buffer;
int timeout; // timeout interval
int polling;          // turnaround delay interval
unsigned int T1_5; // inter character time out in microseconds
unsigned int T3_5; // frame delay in microseconds
unsigned int total_no_of_packets;
Packet* packetArray; // packet starting address
Packet* packet; // current packet

// function definitions
//static void idle();
static void constructPacket();
static unsigned char construct_F15();
static unsigned char construct_F16();
static void waiting_for_reply();
static void processReply();
static void waiting_for_turnaround();
static void process_F1_F2();
static void process_F3_F4();
static void process_F15_F16();
static void processError();
static void processSuccess();
static unsigned int calculateCRC(unsigned char bufferSize);
static void sendPacket(unsigned char bufferSize);

// Modbus Master State Machine
void Modbus_Get(Packet *pkg)
{
    state = IDLE;
    while(state != INVALID)
    {
        switch (state) {
            case IDLE:
                packet = pkg;
                constructPacket();
                break;
            case WAITING_FOR_REPLY:
                waiting_for_reply();
                break;
            case WAITING_FOR_TURNAROUND:
                waiting_for_turnaround();
                break;
            case DONE:
                #ifdef _DEBUG_MODBUS
                do{ unsigned tmp;
                    printf("[%s:%s:%d]: %s[%d]: ", TRACE_GROUP, __FUNCTION__, __LINE__, "rx", packet->data_rcv);
                    if (packet->data_rcv > 0)
                      for (tmp = 0; tmp < packet->data_rcv; tmp++)
                         printf("%04x ", packet->register_array[tmp]);
                    printf("\n");
                } while (0);
                #endif

                packet_index++;
                state = INVALID;
                break;
            case INVALID:
                break;
        }
    }
    
}

void constructPacket()
{
    MODBUS_DEBUG("constructPacket");
    packet->data_rcv = 0;
    
    packet->requests++;
    frame[0] = packet->id;
    frame[1] = packet->function;
    frame[2] = packet->address >> 8; // address Hi
    frame[3] = packet->address & 0xFF; // address Lo
    // For functions 1 & 2 data is the number of points
    // For functions 3, 4 & 16 data is the number of registers
    // For function 15 data is the number of coils
    frame[4] = packet->data >> 8; // MSB
    frame[5] = packet->data & 0xFF; // LSB

    unsigned char frameSize;

    // construct the frame according to the modbus function
    if (packet->function == PRESET_MULTIPLE_REGISTERS)
        frameSize = construct_F16();
    else if (packet->function == FORCE_MULTIPLE_COILS)
        frameSize = construct_F15();
    else // else functions 1,2,3 & 4 is assumed. They all share the exact same request format.
        frameSize = 8; // the request is always 8 bytes in size for the above mentioned functions.

    unsigned int crc16 = calculateCRC(frameSize - 2);
    frame[frameSize - 2] = crc16 >> 8; // split crc into 2 bytes
    frame[frameSize - 1] = crc16 & 0xFF;

    //MODBUS_DUMP("rx", tmp, frameSize, frame);
    #ifdef _DEBUG_MODBUS
    do{ int tmp;
        printf("[%s:%s:%d]: %s[%d]: ", TRACE_GROUP, __FUNCTION__, __LINE__, "tx", buffer);
        for(tmp=0; tmp<frameSize; tmp++)
            printf("%02x ", frame[tmp]);
        printf("\n"); } while (0);
    #endif
                    
    sendPacket(frameSize);
    memset(frame, 0, frameSize);

    state = WAITING_FOR_REPLY; // state change

    // if broadcast is requested (id == 0) for function 15 or 16 then override
    // the previous state and force a success since the slave wont respond
    if (packet->id == 0)
        processSuccess();
}

unsigned char construct_F15()
{
    // function 15 coil information is packed LSB first until the first 16 bits are completed
    // It is received the same way..
    unsigned char no_of_registers = packet->data / 16;
    unsigned char no_of_bytes = no_of_registers * 2;

    // if the number of points dont fit in even 2byte amounts (one register) then use another register and pad
    if (packet->data % 16 > 0) {
        no_of_registers++;
        no_of_bytes++;
    }

    frame[6] = no_of_bytes;
    unsigned char bytes_processed = 0;
    unsigned char index = 7; // user data starts at index 7
    unsigned int temp;

    for (unsigned char i = 0; i < no_of_registers; i++) {
        temp = packet->register_array[i]; // get the data
        frame[index] = temp & 0xFF;
        bytes_processed++;

        if (bytes_processed < no_of_bytes) {
            frame[index + 1] = temp >> 8;
            bytes_processed++;
            index += 2;
        }
    }
    unsigned char frameSize = (9 + no_of_bytes); // first 7 bytes of the array + 2 bytes CRC + noOfBytes
    return frameSize;
}

unsigned char construct_F16()
{
    unsigned char no_of_bytes = packet->data * 2;

    // first 6 bytes of the array + no_of_bytes + 2 bytes CRC
    frame[6] = no_of_bytes; // number of bytes
    unsigned char index = 7; // user data starts at index 7
    unsigned char no_of_registers = packet->data;
    unsigned int temp;

    for (unsigned char i = 0; i < no_of_registers; i++) {
        temp = packet->register_array[i]; // get the data
        frame[index] = temp >> 8;
        index++;
        frame[index] = temp & 0xFF;
        index++;
    }
    unsigned char frameSize = (9 + no_of_bytes); // first 7 bytes of the array + 2 bytes CRC + noOfBytes
    return frameSize;
}

static uint8_t wait_turn_around_display = 0;
void waiting_for_turnaround()
{
    if(!wait_turn_around_display)
    {
        #ifdef _DEBUG_MODBUS
        do{ int tmp;
            printf("[%s:%s:%d]: %s[%d]: ", TRACE_GROUP, __FUNCTION__, __LINE__, "rx", buffer);
            for(tmp=0; tmp<buffer; tmp++)
                printf("%02x ", frame[tmp]);
            printf("\n"); } while (0);
        #endif
            
        MODBUS_DEBUG("waiting_for_turnaround");
        wait_turn_around_display = 1;
    }
    
    if (ModBusTimer.read_ms() > polling)
    {
        wait_turn_around_display = 0;
        state = DONE;
    }
}

// get the serial data from the buffer
void waiting_for_reply()
{
    MODBUS_DEBUG("waiting_for_reply");
    unsigned char overflowFlag = 0;
    unsigned char byte;
    uint8_t lateProcessing = 20;
    buffer = 0;

    do {
        if(ModbusPort.readable() && ModbusPort.read(&byte, 1))
        {
            if (!overflowFlag) {
                if (buffer == BUFFER_SIZE)
                    overflowFlag = 1;

                frame[buffer] = byte;
                buffer++;
            }
        }

        // This is not 100% correct but it will suffice.
        // worst case scenario is if more than one character time expires
        // while reading from the buffer then the buffer is most likely empty
        // If there are more bytes after such a delay it is not supposed to
        // be received and thus will force a frame_error.
        // wait_ms(T1_5); // inter character time out // sophie mark
        if((lateProcessing-- <= 0) &&
            buffer >= 5) {
            // combine the crc Low & High bytes
            unsigned int received_crc = ((frame[buffer - 2] << 8) | frame[buffer - 1]);
            unsigned int calculated_crc = calculateCRC(buffer - 2);
            if (calculated_crc == received_crc) // verify checksum
            {
                MODBUS_DEBUG("frame end");
                break;
            }
        }
    } while(ModBusTimer.read_ms() < timeout);

    // The minimum buffer size from a slave can be an exception response of
    // 5 bytes. If the buffer was partially filled set a frame_error.
    // The maximum number of bytes in a modbus packet is 256 bytes.
    // The serial buffer limits this to 128 bytes.

    if ((buffer < 5) || overflowFlag)
    {
        MODBUS_DEBUG("buffer=%d<5, or overflow=%d", buffer, overflowFlag);
        processError();
    }

    // Modbus over serial line datasheet states that if an unexpected slave
    // responded the master must do nothing and continue with the time out.
    // This seems silly cause if an incorrect slave responded you would want to
    // have a quick turnaround and poll the right one again. If an unexpected
    // slave responded it will most likely be a frame error in any event
    else if (frame[0] != packet->id) // check id returned
    {
        MODBUS_DEBUG("frame[0]=%d != id=%d", frame[0], packet->id);
        processError();
    }
    else
        processReply();
    /*} else 
    if (ModBusTimer.read_ms() > timeout) { // check timeout
        MODBUS_DEBUG("timeout: %d", ModBusTimer.read_ms());
        processError();
        state = IDLE; //state change, override processError() state
    }
    */
}

void processReply()
{
    MODBUS_DEBUG("processReply");
    // combine the crc Low & High bytes
    unsigned int received_crc = ((frame[buffer - 2] << 8) | frame[buffer - 1]);
    unsigned int calculated_crc = calculateCRC(buffer - 2);

    if (calculated_crc == received_crc) { // verify checksum
        // To indicate an exception response a slave will 'OR'
        // the requested function with 0x80
        if ((frame[1] & 0x80) == 0x80) { // extract 0x80
            packet->exception_errors++;
            processError();
        } else {
            switch (frame[1]) { // check function returned
                case READ_COIL_STATUS:
                case READ_INPUT_STATUS:
                    process_F1_F2();
                    break;
                case READ_INPUT_REGISTERS:
                case READ_HOLDING_REGISTERS:
                    process_F3_F4();
                    break;
                case FORCE_MULTIPLE_COILS:
                case PRESET_MULTIPLE_REGISTERS:
                    process_F15_F16();
                    break;
                default: // illegal function returned
                    processError();
                    break;
            }
        }
    } else { // checksum failed
        processError();
    }
}

void process_F1_F2()
{
    // packet->data for function 1 & 2 is actually the number of boolean points
    unsigned char no_of_registers = packet->data / 16;
    unsigned char number_of_bytes = no_of_registers * 2;

    // if the number of points dont fit in even 2byte amounts (one register) then use another register and pad
    if (packet->data % 16 > 0) {
        no_of_registers++;
        number_of_bytes++;
    }

    if (frame[2] == number_of_bytes) { // check number of bytes returned
        unsigned char bytes_processed = 0;
        unsigned char index = 3; // start at the 4th element in the frame and combine the Lo byte
        unsigned int temp;
        for (unsigned char i = 0; i < no_of_registers; i++) {
            temp = frame[index];
            bytes_processed++;
            if (bytes_processed < number_of_bytes) {
                temp = (frame[index + 1] << 8) | temp;
                bytes_processed++;
                index += 2;
            }
            packet->register_array[i] = temp;
        }
        packet->data_rcv = packet->data;
        processSuccess();
    } else // incorrect number of bytes returned
        processError();
}

void process_F3_F4()
{
    // check number of bytes returned - unsigned int == 2 bytes
    // data for function 3 & 4 is the number of registers
    if (frame[2] == (packet->data * 2)) {
        unsigned char index = 3;
        for (unsigned char i = 0; i < packet->data; i++) {
            // start at the 4th element in the frame and combine the Lo byte
            packet->register_array[i] = (frame[index] << 8) | frame[index + 1];
            index += 2;
        }
        packet->data_rcv = packet->data;
        processSuccess();
    } else // incorrect number of bytes returned
        processError();
}

void process_F15_F16()
{
    // Functions 15 & 16 is just an echo of the query
    unsigned int recieved_address = ((frame[2] << 8) | frame[3]);
    unsigned int recieved_data = ((frame[4] << 8) | frame[5]);

    if ((recieved_address == packet->address) && (recieved_data == packet->data))
    {
        packet->data_rcv = packet->data;
        processSuccess();
    }
    else
        processError();
}

void processError()
{
    packet->retries++;
    packet->failed_requests++;

    // if the number of retries have reached the max number of retries
    // allowable, stop requesting the specific packet
    if (packet->retries == retry_count) {
        packet->connection = 0;
        packet->retries = 0;
    }
    state = WAITING_FOR_TURNAROUND;
    ModBusTimer.reset();
    ModBusTimer.start(); // start the turnaround delay
}

void processSuccess()
{
    packet->successful_requests++; // transaction sent successfully
    packet->retries = 0; // if a request was successful reset the retry counter
    state = WAITING_FOR_TURNAROUND;
    ModBusTimer.reset();
    ModBusTimer.start(); // start the turnaround delay
}

void modbus_configure(long _baudrate,
                      int _timeout,
                      int _polling,
                      unsigned char _retry_count,
                      Packet* _packets,
                      unsigned int _total_no_of_packets)
{
    // Modbus states that a baud rate higher than 19200 must use a fixed 750 us
    // for inter character time out and 1.75 ms for a frame delay for baud rates
    // below 19200 the timing is more critical and has to be calculated.
    // E.g. 9600 baud in a 11 bit packet is 9600/11 = 872 characters per second
    // In milliseconds this will be 872 characters per 1000ms. So for 1 character
    // 1000ms/872 characters is 1.14583ms per character and finally modbus states
    // an inter-character must be 1.5T or 1.5 times longer than a character. Thus
    // 1.5T = 1.14583ms * 1.5 = 1.71875ms. A frame delay is 3.5T.
    // Thus the formula is T1.5(us) = (1000ms * 1000(us) * 1.5 * 11bits)/baud
    // 1000ms * 1000(us) * 1.5 * 11bits = 16500000 can be calculated as a constant


    //T1_5 = 750;
    if (_baudrate > 19200)
    {
        T1_5 = 750; 
        T3_5 = 1750; 
    }
    else 
    {
        T1_5 = 15000000/_baudrate; // 1T * 1.5 = T1.5
        T3_5 = 35000000/_baudrate; // 1T * 3.5 = T3.5
    }

    // initialize
    state = IDLE;
    timeout = _timeout;
    polling = _polling;
    retry_count = _retry_count;
    total_no_of_packets = _total_no_of_packets;
    packetArray = _packets;

    ModbusPort.set_baud(_baudrate);
    ModbusPort.set_format(8, SerialBase::Even, 1);

    memset(frame, 0, BUFFER_SIZE);
}

void modbus_construct(Packet *_packet,
                      unsigned char id,
                      unsigned char function,
                      unsigned int address,
                      unsigned int data,
                      unsigned int* register_array)
{
    _packet->id = id;
    _packet->function = function;
    _packet->address = address;
    _packet->data = data;
    _packet->register_array = register_array;
    _packet->connection = 1;
}

unsigned int calculateCRC(unsigned char bufferSize)
{
    unsigned int temp, temp2, flag;
    temp = 0xFFFF;
    for (unsigned char i = 0; i < bufferSize; i++) {
        temp = temp ^ frame[i];
        for (unsigned char j = 1; j <= 8; j++) {
            flag = temp & 0x0001;
            temp >>= 1;
            if (flag)
                temp ^= 0xA001;
        }
    }
    // Reverse byte order.
    temp2 = temp >> 8;
    temp = (temp << 8) | temp2;
    temp &= 0xFFFF;
    // the returned value is already swapped
    // crcLo byte is first & crcHi byte is last
    return temp;
}

void sendPacket(unsigned char bufferSize)
{
    //while(!ModbusPort.writeable());
    //for (unsigned char i = 0; i < bufferSize; i++)
    //    ModbusPort.putc(frame[i]);
    ModbusPort.write(frame, bufferSize);

    //ModbusPort.sync();

    // It may be necessary to add a another character delay T1_5 here to
    // avoid truncating the message on slow and long distance connections

    ModBusTimer.reset();
    ModBusTimer.start(); // start the timeout delay
}

