#ifndef PLAYER_H
#define PLAYER_H

#include <QtDebug>
#include <QThread>
#include <QMessageBox>

#include <alsa/asoundlib.h>

class PlayerWindow;

class MidiPlayer : public QThread {
	//Q_OBJECT

public:
	MidiPlayer(PlayerWindow *parent = 0);
	~MidiPlayer();

	void scanPorts();
	const QStringList getPorts();
	const int getPortIndex() { return port_index; }

	int openPort( int index );
	int openPort();
	int closePort();

	void send_pgmchange( unsigned chan, unsigned value );
	void send_controller( unsigned chan, unsigned param, unsigned value);
	void send_SysEx( const unsigned char *buf, int len );

	int ready();
	unsigned getTick();

	int parseFile(QString &filename);

	void startPlayer();
	void stopPlayer();
	void pausePlayer();
	void resumePlayer();
	void silence();
	void reset();

	void setVolume(int val);

	int queue;

	int currentTick;
	int last_tick;
	double song_length_seconds;

protected:
	virtual void run();

private:
	PlayerWindow *m_parent;

	struct event {
		struct event *next;		// linked list
		unsigned char type;		// SND_SEQ_EVENT_xxx
		unsigned char port;		// port index, generally not used
		unsigned int tick;
		union {
			unsigned char d[3];	// channel and data bytes
			int tempo;
			unsigned int length;	// length of sysex data
		} data;
		std::vector<unsigned char> sysex;
	};  // end struct event definition

	struct track {
		struct event *first_event;	// list of all events in this track
		int end_tick;			// length of this track
		struct event *current_event;	// used while loading and playing
	};  // end struct track definition

	snd_seq_t *seq;
	snd_seq_addr_t port;
	int port_index;

	bool minor_key;
	int sf;  // sharps/flats
	double BPM, PPQ;

	QList<snd_seq_addr_t> ports;
	QList<struct event> all_events;

	void handle_big_sysex(snd_seq_event_t *ev);

	inline void check_snd(const char *, int);
	inline int read_id(void);
	inline int read_byte(void);
	inline void skip(int);
	static bool tick_comp(const struct event& e1, const struct event& e2);
	int read_int(int);
	int read_var(void);
	int read_32_le(void);
	int read_smf(QString &);
	int read_riff(QString &);
	int read_track(int, QString &);
	void play_midi(unsigned int);

	void init_seq();
	void close_seq();
	void connect_port();
	void disconnect_port();
	void getRawDev( const QString &buf = QString() );

	FILE *file;
	int file_offset;
};

// INLINE function
void MidiPlayer::check_snd(const char *operation, int err)
{//qDebug() << "trying " << operation;
	// error handling for ALSA functions
	if (err < 0)
		QMessageBox::critical( static_cast<QWidget *> (m_parent), "MIDI Player", QString("Cannot %1\n%2") .arg(operation) .arg(snd_strerror(err)) );
}
// helper functions, most are INLINE
int MidiPlayer::read_id(void) {
	return read_32_le();
}
int MidiPlayer::read_byte(void) {
	++file_offset;
	return getc(file);
}
void MidiPlayer::skip(int bytes) {
	while (bytes > 0)
		read_byte(), --bytes;
}

#endif // PLAYER_H
