// midi_player.c

#include "playerwindow.h"
#include "player.h"

#include "ui_midi_player.h"

#include <alsa/asoundlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <vector>
#include <algorithm>
#include <QTimer>
#include <QFileDialog>
#include <iostream>

// constructor
PlayerWindow::PlayerWindow(QWidget *parent) :
    QMainWindow(parent),
	ui(new Ui::PlayerWindow)
{
	ui->setupUi(this);
	timer = new QTimer(this);
	connect(timer, SIGNAL(timeout()), this, SLOT(tickDisplay()));

	player = new MidiPlayer( this );

	ui->PortBox->clear();
	ui->PortBox->addItems( player->getPorts() );
	ui->PortBox->setCurrentIndex( player->getPortIndex() );
}   // end constructor

PlayerWindow::~PlayerWindow()
{
	ui->Play_button->setChecked(false);
	delete player;
	delete ui;
}   // end destructor

//  SLOTS
void PlayerWindow::on_Open_button_clicked()
{
	QString fn = QFileDialog::getOpenFileName(this,
		"Open MIDI File", QString(),
		"MIDI files (*.mid *.MID);;Any (*.*)");
	if ( fn.isEmpty() )
		return;

	ui->Play_button->setChecked(false);
	ui->Play_button->setEnabled(false);
	ui->Pause_button->setEnabled(false);
	ui->MidiFile_display->clear();
	player->closePort();

	ui->MidiFile_display->setText(fn);
	ui->MIDI_length_display->setText("00:00");
	player->openPort();
	strcpy(playfile, fn.toLocal8Bit().data());
	if ( !player->parseFile(playfile) ) {
		QMessageBox::critical(this, "MIDI Player", QString("Invalid file"));
		return;
	}   // parseFile
	qDebug() << "last tick: " << player->last_tick;
	ui->progressBar->setRange(0, player->last_tick);
	ui->progressBar->setTickInterval(player->song_length_seconds < 240 ?
										player->last_tick / player->song_length_seconds * 10 :
										player->last_tick / player->song_length_seconds * 30 );
	ui->progressBar->setTickPosition(QSlider::TicksAbove);
	ui->Play_button->setEnabled(true);

	QString time;
	time = QString::number( static_cast<int>(player->song_length_seconds / 60)).rightJustified( 2, '0' );
	time += ":";
	time += QString::number(static_cast<int>(player->song_length_seconds) % 60).rightJustified(2,'0');
	ui->MIDI_length_display->setText( time );

	emit ui->Play_button->setChecked( true );
}   // end on_Open_button_clicked

void PlayerWindow::on_Play_button_toggled(bool checked)
{
	if ( checked )
	{
		ui->Pause_button->setEnabled(true);
		ui->Open_button->setEnabled(false);
		ui->Play_button->setText("Stop");
		ui->progressBar->setEnabled(true);

		player->openPort();
		if ( !player->parseFile(playfile) )
		{
			QMessageBox::critical(this, "MIDI Player", QString("Invalid file"));
			return;
		}   // parseFile
		// queue won't actually start until it is drained
		timer->start(200);
		player->startPlayer();
	}
	else
	{
		if (timer->isActive())
			timer->stop();

		player->stopPlayer();
		player->reset();
		ui->progressBar->blockSignals(true);
		ui->progressBar->setValue(0);
		ui->progressBar->blockSignals(false);
		ui->MIDI_time_display->setText("00:00");
		if (ui->Pause_button->isChecked()) {
			ui->Pause_button->blockSignals(true);
			ui->Pause_button->setChecked(false);
			ui->Pause_button->blockSignals(false);
			ui->Pause_button->setText("Pause");
		}
		ui->Pause_button->setEnabled(false);
		ui->Play_button->setText("Play");
		ui->Open_button->setEnabled(true);
		ui->progressBar->setEnabled(false);
	}
}   // end on_Play_button_toggled

void PlayerWindow::on_Pause_button_toggled(bool checked)
{
	if (checked)
	{
		player->pausePlayer();
		timer->stop();
		ui->Pause_button->setText("Resume");
		qDebug() << "Paused queue" << player->queue << "at tick" << player->currentTick;
	}
	else
	{
		timer->start();
		ui->Pause_button->setText("Pause");
		player->resumePlayer();
		qDebug() << "Playing resumed for queue" << player->queue << "at tick" << player->currentTick;
	}
}   // end on_Pause_button_toggled

void PlayerWindow::on_Panic_button_clicked()
{
	player->silence();
}   // end on_Panic_button_clicked

void PlayerWindow::on_progressBar_sliderPressed()
{
	if ( player->ready() || ui->Pause_button->isChecked() )
		return;
	// stop the timer and queue
	if ( timer->isActive() )
		timer->stop();
	player->pausePlayer();
}   // end on_progressBar_sliderPressed

void PlayerWindow::on_progressBar_sliderReleased()
{
#if 0
	snd_seq_event_t ev;
	snd_seq_ev_clear(&ev);
	snd_seq_ev_set_direct(&ev);
	snd_seq_get_queue_status(seq, queue, status);
	unsigned int current_tick = snd_seq_queue_status_get_tick_time(status);
	const snd_seq_real_time_t *current_time = snd_seq_queue_status_get_real_time(status);
	qDebug() << "Resetting queue from tick" << current_tick << "at" << current_time->tv_sec;
	// reset queue position
	snd_seq_ev_is_tick(&ev);
	snd_seq_ev_set_queue_pos_tick(&ev, queue, 0);
	snd_seq_event_output(seq, &ev);
	snd_seq_drain_output(seq);
	// scan the event queue for the closest tick >= 'x'
	for (std::vector<event>::iterator Event=all_events.begin(); Event!=all_events.end(); ++Event)  {
		if (static_cast<int>(Event->tick) >= ui->progressBar->sliderPosition()) {
			ev.time.tick = Event->tick;
			break;
		}
	}
	ev.dest.client = SND_SEQ_CLIENT_SYSTEM;
	ev.dest.port = SND_SEQ_PORT_SYSTEM_TIMER;
	snd_seq_ev_set_queue_pos_tick(&ev, queue, ev.time.tick);
	snd_seq_event_output(seq, &ev);
	snd_seq_drain_output(seq);
	snd_seq_real_time_t *new_time = new snd_seq_real_time_t;
	double x = static_cast<double>(ev.time.tick)/all_events.back().tick;
	new_time->tv_sec = (x*song_length_seconds);
	new_time->tv_nsec = 0;
	snd_seq_ev_set_queue_pos_real(&ev, queue, new_time);
	// continue the timer
	if (ui->Pause_button->isChecked()) return;
	snd_seq_continue_queue(seq, queue, NULL);
	snd_seq_drain_output(seq);
	current_time = snd_seq_queue_status_get_real_time(status);
	qDebug() << "to tick" << ev.time.tick << "at" << new_time->tv_sec;
	startPlayer(ev.time.tick);
	timer->start();
#endif
}   // end on_progressBar_sliderReleased

void PlayerWindow::tickDisplay() {
	// do timestamp display
	unsigned int current_tick = player->getTick();
	ui->progressBar->blockSignals(true);
	ui->progressBar->setValue(current_tick);
	ui->progressBar->blockSignals(false);
	double new_seconds = static_cast<double>(current_tick) / player->last_tick;
	new_seconds *= player->song_length_seconds;
	ui->MIDI_time_display->setText(QString::number(static_cast<int>(new_seconds)/60).rightJustified(2,'0')+":"+QString::number(static_cast<int>(new_seconds)%60).rightJustified(2,'0'));
	if ( current_tick >= player->last_tick ) {
		sleep(1);
		ui->Play_button->setChecked(false);
	}
}   // end tickDisplay

void PlayerWindow::on_MIDI_Volume_valueChanged(int val) {
	player->setVolume( val );
}

void PlayerWindow::on_PortBox_activated(int index)
{
	qDebug() << "Index changed";

	player->closePort();
	player->openPort( index );
}

void PlayerWindow::on_butRescan_clicked()
{
	player->scanPorts();
	ui->PortBox->clear();
	ui->PortBox->addItems( player->getPorts() );
	ui->PortBox->setCurrentIndex( player->getPortIndex() );
}
