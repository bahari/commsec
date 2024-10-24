/*---------------------------------------------------------------------------*\

  Originally derived from freedv_tx with modifications

\*---------------------------------------------------------------------------*/

/*

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License version 2.1, as
  published by the Free Software Foundation.  This program is
  distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
  License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>
#include <deque>
#include <memory>

#include <jack/jack.h>
#include <samplerate.h>
#include <sndfile.h>

#include "freedv_api.h"

#include "crypto_log.h"
#include "crypto_rx_common.h"
#include "crypto_common.h"
#include "crypto_cfg.h"
#include "resampler.h"
#include "jack_common.h"

static std::unique_ptr<crypto_rx_common> crypto_rx;

static jack_port_t* voice_port = nullptr;
static jack_port_t* modem_port = nullptr;
static jack_port_t* notification_port = nullptr;
static jack_client_t* client = nullptr;

static std::unique_ptr<resampler> input_resampler;
static std::unique_ptr<resampler> output_resampler;

static audio_buffer_t crypto_startup;
static audio_buffer_t plain_startup;
static audio_buffer_t wave_sound;

static std::deque<jack_default_audio_sample_t> notification_buffer;

static volatile sig_atomic_t reload_config = 0;
static volatile sig_atomic_t read_wav = 0;
static volatile sig_atomic_t initialized = 0;
static volatile sig_atomic_t play_wav = 0;

static const char* config_file = nullptr;

static bool read_wav_file(const char* filepath, audio_buffer_t& buffer_out)
{
    const jack_nframes_t jack_sample_rate = jack_get_sample_rate(client);
    return read_wav_file(filepath, jack_sample_rate, buffer_out);
}

static void signal_handler(int sig)
{
    jack_client_close(client);
    fprintf(stderr, "signal received, exiting ...\n");
    exit(0);
}

static void handle_sighup(int sig)
{
    reload_config = 1;
}

static void handle_sigusr1(int sig)
{
    read_wav = 1;
}

/**
 * JACK calls this shutdown_callback if the server ever shuts down or
 * decides to disconnect the client.
 */
void jack_shutdown(void *arg)
{
    exit (1);
}

/**
 * The process callback for this JACK application is called in a
 * special realtime thread once for each audio cycle.
 *
 * This client follows a simple rule: when the JACK transport is
 * running, copy the input port to the output.  When it stops, exit.
 */
int process(jack_nframes_t nframes, void *arg)
{
    const jack_default_audio_sample_t* const modem_frames =
        (jack_default_audio_sample_t*)jack_port_get_buffer(modem_port, nframes);
    bool play_notification_sound = false;
    bool play_wave_sound = false;

    if (initialized != 0) {
        initialized = 0;

        play_notification_sound = true;
    }

    if (play_wav != 0) {
        play_wav = 0;

        play_wave_sound = true;
    }

    const jack_nframes_t jack_sample_rate = jack_get_sample_rate(client);
    const uint voice_sample_rate = crypto_rx->speech_sample_rate();
    const uint modem_sample_rate = crypto_rx->modem_sample_rate();
    const int n_nom_speech_samples = crypto_rx->speech_samples_per_frame();

    const uint modem_resampled_frames =
            get_nom_resampled_frames(crypto_rx->modem_samples_per_frame(),
                                     modem_sample_rate,
                                     jack_sample_rate);

    input_resampler->set_sample_rates(jack_sample_rate, modem_sample_rate);
    output_resampler->set_sample_rates(voice_sample_rate, jack_sample_rate);

    input_resampler->enqueue(modem_frames, nframes);

    const size_t n_max_modem_samples = crypto_rx->max_modem_samples_per_frame();
    const size_t n_max_speech_samples = crypto_rx->max_speech_samples_per_frame();

    size_t nout_this_cycle = 0;
    size_t nin = crypto_rx->needed_modem_samples();
    while (input_resampler->available_elems() >= nin)
    {
        short demod_in[n_max_modem_samples];
        short voice_out[n_max_speech_samples] = {0};

        input_resampler->dequeue(demod_in, nin);

        const size_t nout = crypto_rx->receive(voice_out, demod_in);
        output_resampler->enqueue(voice_out, nout);
        nout_this_cycle += nout;

        /* IMPORTANT: don't forget to do this in the while loop to
           ensure we fread the correct number of samples: ie update
           "nin" before every call to freedv_rx()/freedv_comprx() */
        nin = crypto_rx->needed_modem_samples();
    }

    // If modem data going into the demodulator this cycle
    // results in voice data coming out, or there is no modem
    // data going into the demodulator this cycle (which can
    // happen if) the jack buffer size is smaller than the modem
    // frame size, then consider the output "active"
    // The first time modem data going into the demodulator results
    // in no voice data coming out, that indicates a gap in
    // transmission
    jack_default_audio_sample_t* const voice_frames =
        (jack_default_audio_sample_t*)jack_port_get_buffer(voice_port, nframes);

    // When the radio is active and modem data is coming in we
    // are mostly concerned about having enough data to put onto the
    // voice port during the next time this process runs without
    // underflowing. So make sure the output buffer is "primed"
    // before starting to output data onto the port
    const uint voice_resampled_frames =
        get_nom_resampled_frames(n_nom_speech_samples,
                                 voice_sample_rate,
                                 jack_sample_rate);
    const size_t to_deque = std::min(output_resampler->available_elems(),
                                     (size_t)nframes);
    const size_t to_fill = nframes - to_deque;
    output_resampler->dequeue(voice_frames, to_deque);
    if (to_fill > 0)
    {
        zeroize_frames(voice_frames + to_deque, to_fill);
    }

    if (play_notification_sound)
    {
        const encryption_status crypto_stat = crypto_rx->get_encryption_status();
        if (crypto_stat == CRYPTO_STATUS_ENCRYPTED) {
            notification_buffer.insert(notification_buffer.cend(),
                                       crypto_startup.cbegin(),
                                       crypto_startup.cend());
        }
        else {
            notification_buffer.insert(notification_buffer.cend(),
                                       plain_startup.cbegin(),
                                       plain_startup.cend());
        }
    }

    if (play_wave_sound)
    {
        notification_buffer.insert(notification_buffer.cend(),
                                   wave_sound.cbegin(),
                                   wave_sound.cend());
    }

    jack_default_audio_sample_t* const notification_frames =
        (jack_default_audio_sample_t*)jack_port_get_buffer(notification_port, nframes);
    if (notification_buffer.empty() == false)
    {
        const jack_nframes_t n_notification_frames =
            std::min(nframes, static_cast<jack_nframes_t>(notification_buffer.size()));
        const auto notification_end = notification_buffer.cbegin() + n_notification_frames;
        std::copy(notification_buffer.cbegin(), notification_end, notification_frames);
        notification_buffer.erase(notification_buffer.cbegin(), notification_end);
        if (n_notification_frames < nframes)
        {
            zeroize_frames(notification_frames + n_notification_frames,
                           nframes - n_notification_frames);
        }
    }
    else
    {
        zeroize_frames(notification_frames, nframes);
    }

    return 0;
}

static bool connect_input_ports(jack_port_t* output_port,
                                const char* input_port_regex)
{
    return connect_input_ports(client, output_port, input_port_regex);
}

static void activate_client()
{
    char buffer[128] = {0};
    const struct config* cfg = crypto_rx->get_config();

    const jack_nframes_t jack_sample_rate = jack_get_sample_rate(client);
    const uint speech_sample_rate = crypto_rx->speech_sample_rate();
    const uint speech_samples_per_frame = crypto_rx->speech_samples_per_frame();
    const uint speech_period = get_nom_resampled_frames(speech_samples_per_frame,
                                                        speech_sample_rate,
                                                        jack_sample_rate);

    jack_nframes_t period = get_jack_period(cfg);
    if (period == 0)
    {
        period = speech_period;
        snprintf(buffer,
                 sizeof(buffer),
                 "Buffer size: %u, Speech frame size: %u, Speech sample rate: %u",
                 period,
                 speech_samples_per_frame,
                 speech_sample_rate);
    }
    else
    {
        snprintf(buffer, sizeof(buffer), "Buffer size: %u from config file", period);
    }

    crypto_rx->log_to_logger(LOG_INFO, buffer);
    jack_set_buffer_size(client, period);

    /* Tell the JACK server that we are ready to roll.  Our
     * process() callback will start running now. */
    if (jack_activate (client))
    {
        fprintf (stderr, "cannot activate client");
        exit (1);
    }

    /* Get the port from which we will get data */
    const char* capture_port_name =
        *cfg->jack_modem_in_port ? cfg->jack_modem_in_port : "system:capture_1";
    if (jack_connect(client, capture_port_name, jack_port_name(modem_port)) != 0)
    {
        fprintf(stderr, "Could not connect modem port");
        exit (1);
    }

    const char* voice_playback_port_regex =
        *cfg->jack_voice_out_port ? cfg->jack_voice_out_port : "system:playback_*";
    if (!connect_input_ports(voice_port, voice_playback_port_regex))
    {
        exit(1);
    }

    const char* notify_playback_port_regex =
        *cfg->jack_notify_out_port ? cfg->jack_notify_out_port : "system:playback_*";
    if (!connect_input_ports(notification_port, notify_playback_port_regex))
    {
        exit(1);
    }

    initialized = 1;
}

static void initialize_crypto()
{
    crypto_rx = nullptr;
    input_resampler = nullptr;
    output_resampler = nullptr;

    crypto_rx.reset(new crypto_rx_common("crypto_rx", config_file));

    const jack_nframes_t jack_sample_rate = jack_get_sample_rate(client);
    const uint speech_sample_rate = crypto_rx->speech_sample_rate();
    const uint modem_sample_rate = crypto_rx->modem_sample_rate();

    const size_t speech_frames =
        get_max_resampled_frames(crypto_rx->max_speech_samples_per_frame(),
                                 speech_sample_rate,
                                 jack_sample_rate);
    const size_t modem_frames =
        get_max_resampled_frames(crypto_rx->max_modem_samples_per_frame(),
                                 modem_sample_rate,
                                 jack_sample_rate);

    input_resampler.reset(new resampler(SRC_SINC_FASTEST, 1, modem_frames * 2));
    output_resampler.reset(new resampler(SRC_SINC_FASTEST, 1, speech_frames * 2));

    input_resampler->set_sample_rates(jack_sample_rate, modem_sample_rate);
    output_resampler->set_sample_rates(speech_sample_rate, jack_sample_rate);

    // Pre-initialize the resamplers with null data to "prime" the resampler,
    // then discard the results. The resampler delays the output by some
    // number of samples, and we want to make sure that we always have the
    // same number of bytes available coming out as went in
    input_resampler->enqueue_zeroes(jack_get_buffer_size(client));
    input_resampler->clear();

    output_resampler->enqueue_zeroes(crypto_rx->max_speech_samples_per_frame());
    output_resampler->clear();
}

int main(int argc, char *argv[])
{
    const char* client_name = "crypto_rx";
    const char* server_name = nullptr;
    jack_options_t options = JackNullOption;
    jack_status_t status;

    if (argc > 2)
    {
        server_name = argv[1];
        options = (jack_options_t)(JackNullOption | JackServerName | JackNoStartServer);

        config_file = argv[2];
    }
    else
    {
        fprintf(stderr, "Usage: jack_crypto_rx <jack server name> <config file>");
        exit(1);
    }

    fprintf(stderr, "Server name: %s\n", server_name ? server_name : "");

    /* open a client connection to the JACK server */

    client = jack_client_open (client_name, options, &status, server_name);
    if (client == NULL)
    {
        fprintf (stderr,
                 "jack_client_open() failed, "
                 "status = 0x%2.0x\n",
                 status);
        if (status & JackServerFailed)
        {
            fprintf (stderr, "Unable to connect to JACK server\n");
        }
        exit (1);
    }
    if (status & JackServerStarted)
    {
        fprintf (stderr, "JACK server started\n");
    }
    if (status & JackNameNotUnique)
    {
        client_name = jack_get_client_name(client);
        fprintf (stderr, "unique name `%s' assigned\n", client_name);
    }

    /* tell the JACK server to call `process()' whenever
       there is work to be done.
    */
    jack_set_process_callback (client, process, nullptr);

    /* tell the JACK server to call `jack_shutdown()' if
       it ever shuts down, either entirely, or if it
       just decides to stop calling us.
    */
    jack_on_shutdown (client, jack_shutdown, 0);

    /* create two ports */
    voice_port = jack_port_register(client,
                                    "voice_out",
                                    JACK_DEFAULT_AUDIO_TYPE,
                                    JackPortIsOutput,
                                    0);

    modem_port = jack_port_register(client,
                                    "modem_in",
                                    JACK_DEFAULT_AUDIO_TYPE,
                                    JackPortIsInput,
                                    0);

    notification_port = jack_port_register(client,
                                           "notification_out",
                                           JACK_DEFAULT_AUDIO_TYPE,
                                           JackPortIsOutput,
                                           0);

    if ((voice_port == NULL) || (modem_port == NULL) || (notification_port == NULL))
    {
        fprintf(stderr, "no more JACK ports available\n");
        exit (1);
    }

    try
    {
        initialize_crypto();
    }
    catch (const std::exception& ex)
    {
        fprintf(stderr, "%s", ex.what());
        exit(1);
    }

    const struct config* cfg = crypto_rx->get_config();
    if (cfg->jack_secure_notify_file[0])
    {
        read_wav_file(cfg->jack_secure_notify_file, crypto_startup);
    }
    if (cfg->jack_insecure_notify_file[0])
    {
        read_wav_file(cfg->jack_insecure_notify_file, plain_startup);
    }

    activate_client();

    signal(SIGQUIT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, handle_sighup);
    signal(SIGUSR1, handle_sigusr1);
    signal(SIGINT, signal_handler);

    while (true)
    {
        if (reload_config != 0)
        {
            reload_config = 0;

            jack_deactivate(client);
            try
            {
                initialize_crypto();
            }
            catch (const std::exception& ex)
            {
                fprintf(stderr, "%s", ex.what());
                exit(1);
            }
            activate_client();
        }

        if (read_wav != 0)
        {
            read_wav = 0;

            if (read_wav_file("/tmp/notify.wav", wave_sound))
            {
                play_wav = 1;
            }
        }
        sleep(1);
    }
    
    jack_client_close (client);
    return 0;
}

