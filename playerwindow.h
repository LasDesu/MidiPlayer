#ifndef MIDI_PLAYER_H
#define MIDI_PLAYER_H

#include <QMainWindow>
#include <QTimer>

class MidiPlayer;

namespace Ui {
	class PlayerWindow;
}

class PlayerWindow : public QMainWindow {
	friend class MidiPlayer;

	Q_OBJECT

public:
	PlayerWindow(QWidget *parent = 0);
	~PlayerWindow();

protected:

private:
	Ui::PlayerWindow *ui;

	QTimer *timer;

	MidiPlayer *player;
	QString playfile;

private slots:
	void on_progressBar_sliderReleased();
	void on_progressBar_sliderPressed();
	void on_Pause_button_toggled(bool checked);
	void on_Play_button_toggled(bool checked);
	void on_Panic_button_clicked();
	void on_Open_button_clicked();
	void on_MIDI_Volume_valueChanged(int);
	void tickDisplay();
	void on_PortBox_activated(int index);
	void on_butRescan_clicked();
	void on_butResetGM_clicked();
	void on_butResetGS_clicked();
	void on_butResetXG_clicked();
	void on_butResetMT32_clicked();
};

#endif // MIDI_PLAYER_H
