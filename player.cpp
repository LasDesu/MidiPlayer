// player.cpp   -- part of MIDI_PLAYER
// play memory image midi data to the alsa seq port
// requires access to "seq","queue", "ports" static vars
// contains:
//      check_snd()
//      play_midi()

#include "playerwindow.h"	// hack!
#include "player.h"

#include "ui_midi_player.h" // hack!

#include <QMessageBox>
#include <QTimer>

// FILE global vars
snd_seq_queue_status_t *status;
char playfile[PATH_MAX];
char port_name[16];
char MIDI_dev[16];

MidiPlayer::MidiPlayer( PlayerWindow *parent )
	: QThread( parent )
	, m_parent( parent )
{
	memset( MIDI_dev, 0, sizeof(MIDI_dev) );
	memset( port_name, 0, sizeof(port_name) );
	memset( &port, 0, sizeof(port) );
	seq = NULL;
	queue = 0;
	currentTick = 0;
	port_index = -1;
	port = NULL;

	init_seq();
	queue = snd_seq_alloc_named_queue(seq, "midi_player");
	check_snd("create queue", queue);
	scanPorts(); // empty parm means fill in the PortBox list
	snd_seq_queue_status_malloc(&status);
	//close_seq();

	if ( ports.size() )
	{
		port_index = 0;
		port = &ports[0];
	}
}

MidiPlayer::~MidiPlayer()
{

}

/*
 * 31.25 kbaud, one start bit, eight data bits, two stop bits.
 * (The MIDI spec says one stop bit, but every transmitter uses two, just to be
 * sure, so we better not exceed that to avoid overflowing the output buffer.)
 */
#define MIDI_BYTES_PER_SEC (31250 / (1 + 8 + 2))

void MidiPlayer::handle_big_sysex(snd_seq_event_t *ev)
{
	unsigned int length;
	ssize_t event_size;
	int err;

	length = ev->data.ext.len;
	if (length > MIDI_BYTES_PER_SEC)
		ev->data.ext.len = MIDI_BYTES_PER_SEC;
	event_size = snd_seq_event_length(ev);
	if (event_size + 1 > snd_seq_get_output_buffer_size(seq)) {
		err = snd_seq_drain_output(seq);
		check_snd("drain output", err);
		err = snd_seq_set_output_buffer_size(seq, event_size + 1);
		check_snd("set output buffer size", err);
	}
	while (length > MIDI_BYTES_PER_SEC) {
		err = snd_seq_event_output(seq, ev);
		check_snd("output event", err);
		err = snd_seq_drain_output(seq);
		check_snd("drain output", err);
		err = snd_seq_sync_output_queue(seq);
		check_snd("sync output", err);
		sleep(1);
		ev->data.ext.ptr += MIDI_BYTES_PER_SEC;
		length -= MIDI_BYTES_PER_SEC;
	}
	ev->data.ext.len = length;
}

void MidiPlayer::run()
{
	int end_delay = 2;
	unsigned ch;
	int err;
	// set data in (snd_seq_event_t ev) and output the event
	// common settings for all events
	snd_seq_event_t ev;
	snd_seq_ev_clear(&ev);
	ev.queue = queue;
	ev.source.port = 0;
	ev.flags = SND_SEQ_TIME_STAMP_TICK;
	// parse each event, already in sort order by 'tick' from parse_file
	for ( QList<event>::iterator Event = all_events.begin(); Event != all_events.end(); ++ Event )
	{
		// skip over everything except TEMPO, CONTROLLER, PROGRAM, ChannelPressure and SysEx changes until startTick is reached.
		if ((Event->tick < currentTick) &&
			(Event->type != SND_SEQ_EVENT_TEMPO ||
			 Event->type != SND_SEQ_EVENT_CONTROLLER ||
			 Event->type != SND_SEQ_EVENT_PGMCHANGE ||
			 Event->type != SND_SEQ_EVENT_CHANPRESS ||
			 Event->type != SND_SEQ_EVENT_SYSEX))
		{
			continue;
		}
		ev.time.tick = Event->tick;
		ev.type = Event->type;
//		ev.dest = ports[Event->port];
		ev.dest = *port;
		ch = Event->data.d[0] & 0xF;
		switch ( ev.type ) {
		case SND_SEQ_EVENT_NOTEON:
		case SND_SEQ_EVENT_NOTEOFF:
		case SND_SEQ_EVENT_KEYPRESS:
			snd_seq_ev_set_fixed(&ev);
			ev.data.note.channel = ch;
			ev.data.note.note = Event->data.d[1];
			ev.data.note.velocity = Event->data.d[2];
			break;
		case SND_SEQ_EVENT_CONTROLLER:
			snd_seq_ev_set_fixed(&ev);
			ev.data.control.channel = ch;
			ev.data.control.param = Event->data.d[1];
			ev.data.control.value = Event->data.d[2];
			break;
		case SND_SEQ_EVENT_PGMCHANGE:
		case SND_SEQ_EVENT_CHANPRESS:
			snd_seq_ev_set_fixed(&ev);
			ev.data.control.channel = ch;
			ev.data.control.value = Event->data.d[1];
			break;
		case SND_SEQ_EVENT_PITCHBEND:
			snd_seq_ev_set_fixed(&ev);
			ev.data.control.channel = ch;
			ev.data.control.value = Event->data.d[1];
			ev.data.control.value |= Event->data.d[2] << 7;
			ev.data.control.value -= 0x2000;
			break;
		case SND_SEQ_EVENT_SYSEX:
			snd_seq_ev_set_variable(&ev, Event->data.length, Event->sysex.data());
			handle_big_sysex(&ev);
			break;
		case SND_SEQ_EVENT_TEMPO:
			snd_seq_ev_set_fixed(&ev);
			ev.dest.client = SND_SEQ_CLIENT_SYSTEM;
			ev.dest.port = SND_SEQ_PORT_SYSTEM_TIMER;
			ev.data.queue.queue = queue;
			ev.data.queue.param.value = Event->data.tempo;
			break;
		default:
			QMessageBox::critical( m_parent, "MIDI Player", QString("Invalid event type %1") .arg(ev.type) );
		}	// end SWITCH ev.type
		// do the actual output of the event to the MIDI queue
		// this blocks when the output pool has been filled
		err = snd_seq_event_output(seq, &ev);
		check_snd("output event", err);
	}	// end for all_events iterator

	// schedule queue stop at end of song
	snd_seq_ev_set_fixed(&ev);
	ev.type = SND_SEQ_EVENT_STOP;
	if ( all_events.size() )
		ev.time.tick = all_events.back().tick;
	else
		ev.time.tick = 0;
	ev.dest.client = SND_SEQ_CLIENT_SYSTEM;
	ev.dest.port = SND_SEQ_PORT_SYSTEM_TIMER;
	ev.data.queue.queue = queue;
	err = snd_seq_event_output(seq, &ev);
	check_snd("output event", err);
	// make sure that the sequencer sees all our events
	err = snd_seq_drain_output(seq);
	check_snd("drain output", err);

	// There are three possibilities for how to wait until all events have been played:
	// 1) send an event back to us (like pmidi does), and wait for it;
	// 2) wait for the EVENT_STOP notification for our queue which is sent
	//    by the system timer port (this would require a subscription);
	// 3) wait until the output pool is empty.
	// The last is the simplest.
	err = snd_seq_sync_output_queue(seq);
	check_snd("sync output", err);
	// give the last notes time to die away
	if (end_delay > 0)
		sleep(end_delay);
}	// end play_midi


//  FUNCTIONS
void MidiPlayer::send_pgmchange( unsigned chan, unsigned value)
{
	snd_seq_event_t ev;

	snd_seq_ev_clear(&ev);
	ev.dest = *port;

	snd_seq_ev_set_pgmchange( &ev, chan, value );
	snd_seq_ev_set_direct(&ev);

	snd_seq_event_output_direct(seq, &ev);
	//snd_seq_drain_output(seq);
}

void MidiPlayer::send_controller( unsigned chan, unsigned param, unsigned value)
{
	snd_seq_event_t ev;
	int ret;

	snd_seq_ev_clear(&ev);
	ev.dest = *port;

	snd_seq_ev_set_controller( &ev, chan, param, value );
	ret = snd_seq_ev_set_direct(&ev);

	snd_seq_event_output_direct(seq, &ev);
	//snd_seq_drain_output(seq);
}

void MidiPlayer::send_SysEx(char * buf,int data_size)
{
	pausePlayer();
	snd_seq_event_t ev;
	snd_seq_ev_clear(&ev);
	ev.dest = *port;
	snd_seq_ev_set_sysex(&ev, data_size, buf );
	snd_seq_ev_set_direct(&ev);
	snd_seq_event_output_direct(seq, &ev);
	snd_seq_drain_output(seq);
	resumePlayer();
}	// end send_SysEx

void MidiPlayer::init_seq()
{
	if (!seq) {
		int err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0);
		check_snd("open sequencer", err);
		err = snd_seq_set_client_name(seq, "midi_player");
		check_snd("set client name", err);
		int client = snd_seq_client_id(seq);    // client # is 128 by default
		check_snd("get client id", client);
		qDebug() << "Seq and client initialized";
	}
}

void MidiPlayer::close_seq()
{
	if (seq) {
		snd_seq_stop_queue( seq, queue, NULL );
		snd_seq_drop_output( seq );
		snd_seq_drain_output( seq );
		snd_seq_close( seq );
		seq = 0;
		qDebug() << "Seq closed";
	}
}

void MidiPlayer::connect_port()
{
	if ( seq && (port_index >= 0)) {
		//  create_source_port
		snd_seq_port_info_t *pinfo;
		snd_seq_port_info_alloca(&pinfo);
		// the first created port is 0 anyway, but let's make sure ...
		snd_seq_port_info_set_port(pinfo, 0);
		snd_seq_port_info_set_port_specified(pinfo, 1);
		snd_seq_port_info_set_name(pinfo, "midi_player");
		snd_seq_port_info_set_capability(pinfo, 0);
		snd_seq_port_info_set_type(pinfo,
			SND_SEQ_PORT_TYPE_MIDI_GENERIC |
			SND_SEQ_PORT_TYPE_APPLICATION);
		int err = snd_seq_create_port(seq, pinfo);
		check_snd("create port", err);

		port = &ports[port_index];
		err = snd_seq_connect_to(seq, 0, port->client, port->port );
		if (err < 0 && err!= -16)
			QMessageBox::critical(m_parent, "MIDI Player", QString("%4 Cannot connect to port %1:%2 - %3") .arg(port->client) .arg(port->port) .arg(strerror(errno)) .arg(err));
		qDebug() << "Connected port" << port->client << ":" << port->port ;
	}
}	// end connect_port

void MidiPlayer::disconnect_port()
{
	if ( seq && port ) {
		int err;
		err = snd_seq_disconnect_to(seq, 0, port->client, port->port );
		qDebug() << "Disconnected current port" << port->client << ":" << port->port;
	}	// end if seq
}	// end disconnect_port

void MidiPlayer::scanPorts()
{
	// fill in the combobox with all available ports
	// or set port_name to the port passed in buf
	snd_seq_client_info_t *cinfo;
	snd_seq_port_info_t *pinfo;
	snd_seq_client_info_alloca(&cinfo);
	snd_seq_port_info_alloca(&pinfo);
	snd_seq_client_info_set_client(cinfo, -1);

	ports.clear();

	while (snd_seq_query_next_client(seq, cinfo) >= 0) {
		int client = snd_seq_client_info_get_client(cinfo);
		snd_seq_port_info_set_client(pinfo, client);
		snd_seq_port_info_set_port(pinfo, -1);
		while (snd_seq_query_next_port(seq, pinfo) >= 0) {
			/* we need both WRITE and SUBS_WRITE */
			if ((snd_seq_port_info_get_capability(pinfo)
				 & (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE))
				!= (SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE))
				continue;

			ports << *snd_seq_port_info_get_addr(pinfo);
		}
	}
}	// end getPorts

const QStringList MidiPlayer::getPorts()
{
	snd_seq_port_info_t *pinfo;
	QStringList names;
	int i;

	snd_seq_port_info_alloca(&pinfo);

	for ( i = 0; i < ports.size(); i ++ )
	{
		snd_seq_get_any_port_info( seq, ports[i].client, ports[i].port, pinfo );
		names << snd_seq_port_info_get_name( pinfo );
	}

	return names;
}

void MidiPlayer::getRawDev( const QString &buf ) {
	signed int card_num = -1;
	signed int dev_num = -1;
	signed int subdev_num = -1;
	int err, i;
	char str[64];
	snd_rawmidi_info_t *rawMidiInfo;
	snd_ctl_t *cardHandle;

	if ( buf.isEmpty() )
		return;

	err = snd_card_next( &card_num );
	if (err < 0)
	{
		memset(MIDI_dev,0,sizeof(MIDI_dev));
		// no MIDI cards found in the system
		snd_card_next(&card_num);
		return;
	}
	while (card_num >= 0) {
		sprintf( str, "hw:%i", card_num );
		if ( (err = snd_ctl_open(&cardHandle, str, 0)) < 0 )
			break;
		dev_num = -1;
		err = snd_ctl_rawmidi_next_device(cardHandle, &dev_num);
		if ( err < 0 ) {
			// card exists, but no midi device was found
			snd_card_next(&card_num);
			continue;
		}
		while (dev_num >= 0) {
			snd_rawmidi_info_alloca(&rawMidiInfo);
			memset(rawMidiInfo, 0, snd_rawmidi_info_sizeof());
			// Tell ALSA which device (number) we want info about
			snd_rawmidi_info_set_device(rawMidiInfo, dev_num);
			// Get info on the MIDI outs of this device
			snd_rawmidi_info_set_stream(rawMidiInfo, SND_RAWMIDI_STREAM_OUTPUT);
			i = -1;
			subdev_num = 1;
			// More subdevices?
			while (++i < subdev_num) {
				// Tell ALSA to fill in our snd_rawmidi_info_t with info on this subdevice
				snd_rawmidi_info_set_subdevice(rawMidiInfo, i);
				if ( (err = snd_ctl_rawmidi_info(cardHandle, rawMidiInfo)) < 0 )
					continue;
				// Print out how many subdevices (once only)
				if ( !i )
					subdev_num = snd_rawmidi_info_get_subdevices_count(rawMidiInfo);

				// got a valid card, dev and subdev
				if (buf == (QString)snd_rawmidi_info_get_subdevice_name(rawMidiInfo)) {
					QString holdit = "hw:" + QString::number(card_num) + "," + QString::number(dev_num) + "," + QString::number(i);
					strcpy(MIDI_dev, holdit.toLocal8Bit().data());
				}
			}	// end WHILE subdev_num
			snd_ctl_rawmidi_next_device(cardHandle, &dev_num);
		}	// end WHILE dev_num
		snd_ctl_close(cardHandle);
		err = snd_card_next(&card_num);
	}	// end WHILE card_num
}	// end getRawDev()

int MidiPlayer::openPort( int index )
{
	init_seq();
	port_index = index;
	connect_port();

	return 0;
}

int MidiPlayer::openPort()
{
	init_seq();
	queue = snd_seq_alloc_named_queue(seq, "midi_player");
	check_snd("create queue", queue);
	connect_port();

	return 0;
}

int MidiPlayer::closePort()
{
	disconnect_port();
	//close_seq();

	return 0;
}

int MidiPlayer::ready()
{
	if ( !seq )
		return 0;
	if ( !queue )
		return 0;

	return 1;
}

unsigned MidiPlayer::getTick()
{
	snd_seq_get_queue_status(seq, queue, status);
	return snd_seq_queue_status_get_tick_time(status);
}

void MidiPlayer::startPlayer()
{
	init_seq();
	connect_port();
	// queue won't actually start until it is drained
	int err = snd_seq_start_queue(seq, queue, NULL);
	check_snd("start queue", err);
	currentTick = 0;
	start();
}

void MidiPlayer::stopPlayer()
{
	if ( isRunning() )
		terminate();
	snd_seq_drop_output(seq);
	snd_seq_drain_output(seq);
}

void MidiPlayer::resumePlayer()
{
	snd_seq_continue_queue(seq, queue, NULL);
	snd_seq_drain_output(seq);
	start();
}

void MidiPlayer::pausePlayer()
{
	stopPlayer();
	snd_seq_stop_queue(seq, queue, NULL);
	snd_seq_get_queue_status(seq, queue, status);
	currentTick = snd_seq_queue_status_get_tick_time(status);
	snd_seq_drain_output(seq);
	silence();
}

void MidiPlayer::silence()
{
	if ( seq )
	{
		connect_port();
		for ( int x = 0; x < 16; x ++ )
		{
			send_controller( x, 123, 0 );
			send_controller( x, 120, 0 );
		}
	}
	else
	{
		char buf[6];
		getRawDev(m_parent->ui->PortBox->currentText());
		if (strlen(MIDI_dev)) {
			snd_rawmidi_t *midiInHandle;
			snd_rawmidi_t *midiOutHandle;
			int err=snd_rawmidi_open(&midiInHandle, &midiOutHandle, MIDI_dev, 0);
			check_snd("open rawidi",err);
			snd_rawmidi_nonblock(midiInHandle, 0);
			err = snd_rawmidi_read(midiInHandle, NULL, 0);
			check_snd("read rawidi",err);
			snd_rawmidi_drop(midiOutHandle);
			buf[0] = 0xCC;
			buf[1] = 123;
			err = snd_rawmidi_write(midiOutHandle, buf, 2);
			for ( int x = 0; x < 16 ; x ++ )
			{
				buf[0] = buf[3] = 0xC0+x;
				buf[1] = 0x7B;
				buf[4] = 0x79;
				buf[2] = buf[5] = 00;
				err = snd_rawmidi_write(midiOutHandle, buf, 6);
			}
			snd_rawmidi_drain(midiOutHandle);
			snd_rawmidi_close(midiOutHandle);
			snd_rawmidi_close(midiInHandle);
		} // end strlen(MIDI_dev)
	} // end else
}	// end on_Panic_button_clicked

void MidiPlayer::reset()
{
	silence();
	for ( int x = 0; x < 16; x ++ )
	{
		send_controller( x, 121, 0 );
	}
}

void MidiPlayer::setVolume(int val) {
	char buf[8];
	if (seq) {
		connect_port();
		buf[0] = 0xF0;
		buf[1] = 0x7F;
		buf[2] = 0x7F;
		buf[3] = 0x04;
		buf[4] = 0x01;
		buf[5] = 0x00;
		buf[6] = val;
		buf[7] = 0xF7;
		send_SysEx(buf, 8);
	}
}
