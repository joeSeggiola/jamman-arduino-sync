
// CONFIGURATION =============================================================

const HardwareSerial *JM = &Serial; // Serial port for JM sync cable (MIDI-like), eg. &Serial1 on secondary serial port on Mega
const HardwareSerial *DEBUG = 0; // &Serial to debug on USB, or zero to disable debugging

const int PULSE_INTERRUPT_PIN = 3; // Digital pin for pulse clock input

const bool RESET_BUTTON_ENABLE = true; // Enable the button to force next quarter on new measure
const int RESET_BUTTON_PIN = 5; // Digital pin for reset button
const unsigned int RESET_BUTTON_POLL_MS = 20; // Reset button polling interval

const int LED_PLAYING_PIN = 13; // Playing/stop LED
const int LED_BEAT_M = 12; // Measure LED (quarter 1)
const int LED_BEAT_Q = 11; // Quarter LED (quarter 2, 3, 4)
const int LEDS_DURATION_MS = 80; // LED flashing duration

const unsigned int QPM = 4; // Quarters per measure

const bool PPQ_OPT_SWITCH_ENABLE = false; // Enable the advanced PPQ switch
const unsigned int PPQ_OPT_1 = 1; // Incoming pulses per quarter (option 1, default)
const unsigned int PPQ_OPT_2 = 4; // Incoming pulses per quarter (option 2)
const unsigned int PPQ_OPT_SWITCH_PIN = 7; // PPQ switch analog pin (0V => option 1, 5V => option 2)
const unsigned int PPQ_OPT_SWITCH_POLL_MS = 500; // PPQ switch polling interval

const unsigned int STOP_Q = 3; // How many expected quarters missing to detect clock stopped
const unsigned int STOP_MIN_MS = 1500; // Minimum time for clock stop detection, used if calculated STOP_Q is smaller: this fixes the "stop detection loop" when clock changes suddenly from very fast to very slow

const unsigned int BPM_LIMIT_MIN = 40; // Minimum BPM, warn under this value
const unsigned int BPM_LIMIT_MAX = 240; // Maximum BPM, warn over this value
const unsigned int BPM_LIMIT_WARNING_FLASH_MS = 70; // Flashing period for BPM warning on playing LED

const bool DISPLAY_ENABLE = false; // BPM display with LTC-2727G
const int DISPLAY_CATHODES[] {10, 11, 12}; // Common cathodes pins for three digits
const int DISPLAY_ANODES[] {7, 8, 3, 5, 4, 9, 6}; // Anodes pins for 7-segments digits

// ===========================================================================

const int JM_LINK_PERIOD_MS = 400;

// JM sync signal packets, from:
// http://fuzzysynth.blogspot.com/2015/06/digitech-jam-man.html
// https://github.com/Calde-github/Looperino/blob/master/Looper.ino
const byte JM_SYNC[] {0xF0, 0x00, 0x00, 0x10, 0x7F, 0x62, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x04, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7};
const byte JM_LINK[] {0xF0, 0x00, 0x00, 0x10, 0x7F, 0x62, 0x01, 0x00, 0x01, 0x01, 0xF7};

bool playing = false;

unsigned int pulseCounter = 0;
unsigned int quarterCounter = 0;
unsigned long quarterLastMicros = 0;
unsigned long measureDurationMicros = 0;
unsigned long measureLastMicros = 0;
unsigned int bpm = 0; // Computed BPM

volatile bool pulseFlag = false; // A pulse has been detected in the ISR
volatile bool resetFlag = false; // The reset button has been pressed

unsigned int PPQ = PPQ_OPT_1; // Incoming pulses per quarter (selected option)
unsigned long linkLastMillis = 0;
unsigned long resetButtonLastMillis = 0;
unsigned long ppqSwitchLastMillis = 0;

void setup() {

	// Debugging
	if (DEBUG) DEBUG->begin(9600);
	
	// Reset button setup
	pinMode(RESET_BUTTON_PIN, INPUT);

	// Status LEDs setup
	pinMode(LED_PLAYING_PIN, OUTPUT);
	pinMode(LED_BEAT_M, OUTPUT);
	pinMode(LED_BEAT_Q, OUTPUT);
	digitalWrite(LED_PLAYING_PIN, LOW);
	digitalWrite(LED_BEAT_M, LOW);
	digitalWrite(LED_BEAT_Q, LOW);
	
	// Display setup
	if (DISPLAY_ENABLE) {
		for (int i = 0; i < 3; i++) pinMode(DISPLAY_CATHODES[i], OUTPUT);
		for (int i = 0; i < 7; i++) pinMode(DISPLAY_ANODES[i], OUTPUT);
	}
	
	// JM MIDI-like out
	JM->begin(31250);

	// Send JM link
	linkMaintain();

	// Attach pulses interrupt
	attachInterrupt(digitalPinToInterrupt(PULSE_INTERRUPT_PIN), pulseFlagSet, RISING);

}

void loop() {
	resetButton();
	ppqSwitch();
	pulseFlagProcess();
	detectStop();
	linkMaintain();
	leds();
	display();
}

void resetButton() {
	if (RESET_BUTTON_ENABLE) {
		if ((linkLastMillis == 0) || (millis() - resetButtonLastMillis > RESET_BUTTON_POLL_MS)) {
			if (digitalRead(RESET_BUTTON_PIN)) resetFlag = true;
			resetButtonLastMillis = millis();
		}
	}
}

void ppqSwitch() {
	if (PPQ_OPT_SWITCH_ENABLE) {
		if ((linkLastMillis == 0) || (millis() - ppqSwitchLastMillis > PPQ_OPT_SWITCH_POLL_MS)) {
			int ppqSwitchValue = analogRead(PPQ_OPT_SWITCH_PIN);
			PPQ = ppqSwitchValue < 512 ? PPQ_OPT_1 : PPQ_OPT_2;
			ppqSwitchLastMillis = millis();
		}
	}
}

void pulseFlagSet() {
	pulseFlag = true;
}

void pulseFlagProcess() {
	
	if (pulseFlag) {
		pulseFlag = false;

		unsigned long nowMicros = micros();

		// If it was stopped, resume
		if (!playing || resetFlag) {
			resetFlag = false;

			if (DEBUG) DEBUG->println("Resuming...");

			// Fake last measure in order to keep last known tempo
			if (measureDurationMicros > 0) {
				measureLastMicros = nowMicros - measureDurationMicros;
			}

			pulseCounter = 0;
			quarterCounter = 0;
			quarterLastMicros = nowMicros;
			playing = true;
			if (DEBUG) DEBUG->println("Playing.");

		} else {

			// Count pulse
			pulseCounter = (pulseCounter + 1) % PPQ;

			// New quarter?
			if (pulseCounter == 0) {
				quarterCounter = (quarterCounter + 1) % QPM;
				quarterLastMicros = nowMicros;
				if (DEBUG) {
					DEBUG->print("Got quarter: ");
					DEBUG->println(quarterCounter);
				}
			}

		}

		// New measure?
		if (pulseCounter == 0 && quarterCounter == 0) {

			// Compute BPM
			if (measureLastMicros > 0) { // Got the first measure ever?
				if (DEBUG) DEBUG->println("Got measure.");
				measureDurationMicros = nowMicros - measureLastMicros;
				bpm = round(QPM * 60000000.0 / measureDurationMicros);
				if (DEBUG) {
					DEBUG->print("Sending sync with tempo: ");
					DEBUG->print(bpm);
					DEBUG->println(" BPM");
				}

				// Send JM sync signal with BPM and measure duration information
				syncSend();

			} else {
				if (DEBUG) DEBUG->println("Got first measure ever.");
			}

			measureLastMicros = nowMicros;

		}

	}
	
}

void detectStop() {
	
	// Detect if stopped (3 quarters missing, and last one more than 1.5 seconds ago)
	if (playing) {
		if (measureDurationMicros > 0) { // I know how much a measure is expected to be long
			unsigned long quarterExpectedDuration = measureDurationMicros / QPM;
			unsigned long timeSinceLastQuarter = micros() - quarterLastMicros;
			if ((timeSinceLastQuarter > STOP_Q * quarterExpectedDuration) && (timeSinceLastQuarter / 1000 > STOP_MIN_MS)) {
				playing = false;
				if (DEBUG) DEBUG->println("Stop detected!");
			}
		}
	}
	
}

void leds() {
	
	// Playing indicator
	if (playing) {
		if (bpm > 0 && (bpm < BPM_LIMIT_MIN || bpm > BPM_LIMIT_MAX)) {
			digitalWrite(LED_PLAYING_PIN, (millis() / BPM_LIMIT_WARNING_FLASH_MS) % 2 ? HIGH : LOW);
		} else {
			digitalWrite(LED_PLAYING_PIN, HIGH);
		}
	} else {
		digitalWrite(LED_PLAYING_PIN, LOW);
	}
	
	// Beat indicators
	bool ledsOn = playing && ((micros() - quarterLastMicros) / 1000 < LEDS_DURATION_MS);
	digitalWrite(LED_BEAT_M, ledsOn && quarterCounter == 0 ? HIGH : LOW);
	digitalWrite(LED_BEAT_Q, ledsOn ? HIGH : LOW);
	
}

void linkMaintain() {
	
	// Keep JM link active by sending the link packet every ~400ms
	if (millis() - linkLastMillis > JM_LINK_PERIOD_MS) {
		JM->write(JM_LINK, sizeof(JM_LINK));
		linkLastMillis = millis();	
	}
	
}

void syncSend() {

	// From:
	// https://github.com/Calde-github/Looperino/blob/master/Looper.ino

	// Copy base JM sync packet
	int syncPacketSize = sizeof(JM_SYNC);
	byte syncPacket[syncPacketSize];
	for (int i = 0; i < syncPacketSize; i++) syncPacket[i] = JM_SYNC[i];

	// BPM
	syncPacket[7] = 66 + 8 * ((63 < bpm) && (bpm < 128) || bpm > 191) ;
	syncPacket[11] = (4 * bpm > 127 && 4 * bpm < 256) * (4 * bpm - 128) +
	                 (2 * bpm > 127 && 2 * bpm < 256) * (2 * bpm - 128) +
	                 (bpm > 127 && bpm < 256) * (bpm - 128);
	syncPacket[12] = 1 * (bpm > 127) + 66;

	// Measure length
	unsigned long loopTime = floor(measureDurationMicros / 1000.0);
	int x = floor(log(loopTime / 2000.0) / log(4.0));
	int b163 = (loopTime / (2000.0 * pow(4.0, x))) > 2;
	int y = 2 * pow(2, b163) * pow(4, x);
	int w = floor(loopTime / y);
	syncPacket[15] = 64 + 8 * b163;
	syncPacket[20] = 64 + x;
	syncPacket[19] = 128 * (0.001 * w - 1);
	syncPacket[18] = pow(128.0, 2) * (0.001 * w - 1 - syncPacket[19] / 128.0);
	syncPacket[17] = pow(128.0, 3) * (0.001 * w - 1 - syncPacket[19] / 128.0 - syncPacket[18] / pow(128.0, 2));

	// Command (SYNC)
	syncPacket[21] = 5;

	// Checksum XOR
	byte z = 0;
	for (int i = 7; i < 22; i++) z = z ^ syncPacket[i];
	syncPacket[22] = z;

	JM->write(syncPacket, syncPacketSize);

}

void display() {
	if (DISPLAY_ENABLE) {
		displayNumber(playing && bpm > 0 ? bpm : -1);
	}
}

void displayNumber(int number) {
	displayDigit(2, number < 0 ? -1 : (number) % 10);
	displayDigit(1, number < 0 ? -1 : (number / 10) % 10);
	displayDigit(0, number < 0 ? -1 : (number / 100) % 10);
}

void displayDigit(int digitIndex, int digit) {
	for (int i = 0; i < 3; i++) digitalWrite(DISPLAY_CATHODES[i], HIGH); // Turn all off
	digitalWrite(DISPLAY_ANODES[0], digit >= 0 && (digit != 1 && digit != 4) ? HIGH : LOW);
	digitalWrite(DISPLAY_ANODES[1], digit >= 0 && (digit != 5 && digit != 6) ? HIGH : LOW);
	digitalWrite(DISPLAY_ANODES[2], digit >= 0 && (digit != 2) ? HIGH : LOW);
	digitalWrite(DISPLAY_ANODES[3], digit >= 0 && (digit != 1 && digit != 4 && digit != 7) ? HIGH : LOW);
	digitalWrite(DISPLAY_ANODES[4], digit >= 0 && (digit == 0 || digit == 2 || digit == 6 || digit == 8) ? HIGH : LOW);
	digitalWrite(DISPLAY_ANODES[5], digit >= 0 && (digit != 1 && digit != 2 && digit != 3 && digit != 7) ? HIGH : LOW);
	digitalWrite(DISPLAY_ANODES[6], digit >= 0 && (digit != 0 && digit != 1 && digit != 7) ? HIGH : LOW);
	digitalWrite(DISPLAY_CATHODES[digitIndex], LOW); // Turn digit on
}


