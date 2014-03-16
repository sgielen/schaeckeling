#include <assert.h>
#include <stdio.h>
#include "dmxdriver.h"
#include "dmxd.h"

struct mk2_pro_context *mk2c;
volatile int mk2c_lost = 0;
volatile int nanokontrol_lost = 0;
volatile int midi_lost = 0;

volatile int receiving_changes = 0;


void
midi_changed(int channel, unsigned char value) {
	assert(channel >= 0 && channel < MIDI_CHANNELS);
	fprintf(stdout, "midi_changed(%d, %d)\n", channel, (int)value);
	receiving_changes = 1;
	update_channel(channel + DMX_CHANNELS, value);
}


void
midi_input_completed(void) {
	receiving_changes = 0;
	flush_dmx2_sendbuf();
//	update_websockets(0, 1);
}


void
dmx_changed(int channel, unsigned char old, unsigned char new) {
	assert(channel >= 0 && channel < DMX_CHANNELS);
	fprintf(stdout, "dmx_changed(%d, %d, %d)\n", channel, (int)old, (int)new);
	receiving_changes = 1;
	update_channel(channel, new);
}


void
dmx_input_completed(void) {
	receiving_changes = 0;
	flush_dmx2_sendbuf();
	update_websockets(0, 1);
}


void
mk2c_error(int error) {
	fprintf(stderr, "mk2c_error: %d\n", error);
	mk2c_lost = 1;
	error_step();
}


void
nanokontrol_error(int error) {
	fprintf(stderr, "nanokontrol_error: %d\n", error);
	nanokontrol_lost = 1;
	error_step();
}


void
generic_midi_error(int error) {
	fprintf(stderr, "generic_midi_error: %d\n", error);
	midi_lost = 1;
	error_step();
}


void
reconnect_if_needed(void) {
	if (mk2c_lost) {
		if (mk2c != NULL) {
			teardown_dmx_usb_mk2_pro(mk2c);
		}
		mk2c = init_dmx_usb_mk2_pro(dmx_changed, dmx_input_completed, mk2c_error);
		if (mk2c != NULL) {
			mk2c_lost = 0;
			flush_dmx2_sendbuf();
		}
	}
	if (midi_lost) {

	}
	if (nanokontrol_lost) {

	}
}

int
init_communications(void) {
	mk2c = init_dmx_usb_mk2_pro(dmx_changed, dmx_input_completed, mk2c_error);
	if(mk2c == NULL) {
		abort(); // XXX
	}
	// Fix nanokontrol and usb-midi.
	return 0;
}


int
send_dmx(unsigned char *dmxbytes) {
	int ret = -2;
	if (mk2c != NULL) {
		ret = mk2_send_dmx(mk2c, dmxbytes);
	}
	return ret;
}