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
#include <sys/wait.h>
#include <unistd.h>

#include <vector>
#include <deque>
#include <memory>

#include <gpiod.h>

#include <jack/jack.h>

#include "freedv_api.h"

#include "resampler.h"
#include "crypto_cfg.h"
#include "crypto_log.h"
#include "crypto_tx_common.h"
#include "crypto_common.h"
#include "jack_common.h"

static std::unique_ptr<crypto_tx_common> crypto_tx;

static jack_port_t* voice_port = nullptr;
static jack_port_t* modem_port = nullptr;
static jack_client_t* client = nullptr;

static std::unique_ptr<resampler> input_resampler;
static std::unique_ptr<resampler> output_resampler;

static audio_buffer_t tts_file;
static std::deque<jack_default_audio_sample_t> tts_buffer;

static volatile sig_atomic_t reload_config = 0;
static volatile sig_atomic_t read_wav = 0;
static volatile sig_atomic_t play_wav = 0;

static volatile sig_atomic_t sig_ptt_val = 0;

static const char* config_file = nullptr;

static struct gpiod_line* ptt_in_line = nullptr;
static struct gpiod_line* ptt_out_line = nullptr;

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

static void handle_sigptt(int sig)
{
    sig_ptt_val = !sig_ptt_val;
}

static bool microphone_enabled(const struct config* cfg)
{
    if (cfg->ptt_enabled && cfg->ptt_gpio_num < 0)
    {
        return sig_ptt_val != 0;
    }
    else if (ptt_in_line != nullptr)
    {
        const int result = gpiod_line_get_value(ptt_in_line);
        if (result < 0)
        {
            crypto_tx->log_to_logger(LOG_ERROR, "Error reading PTT IO");
            return true;
        }
        else
        {
            return result;
        }
    }
    else
    {
        return true;
    }
}

static void set_ptt_val(const struct config* cfg, bool val)
{
    static int prev_val = -1;
    const int cur_val = static_cast<int>(val);

    if (ptt_out_line == nullptr)
    {
        prev_val = -1;
        return;
    }
    else if (prev_val != cur_val &&
             gpiod_line_set_value(ptt_out_line, cur_val) == 0)
    {
        prev_val = cur_val;
    }
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
    const jack_default_audio_sample_t* const voice_frames =
        (jack_default_audio_sample_t*)jack_port_get_buffer(voice_port, nframes);
    jack_default_audio_sample_t* const modem_frames =
            (jack_default_audio_sample_t*)jack_port_get_buffer(modem_port, nframes);

    const jack_nframes_t jack_sample_rate = jack_get_sample_rate(client);
    const uint voice_sample_rate = crypto_tx->speech_sample_rate();
    const uint modem_sample_rate = crypto_tx->modem_sample_rate();

    input_resampler->set_sample_rates(jack_sample_rate, voice_sample_rate);
    output_resampler->set_sample_rates(modem_sample_rate, jack_sample_rate);

    const struct config* cfg = crypto_tx->get_config();

    const size_t n_nom_modem_samples = crypto_tx->modem_samples_per_frame();
    const size_t n_speech_samples = crypto_tx->speech_samples_per_frame();

    if (play_wav != 0)
    {
        play_wav = 0;

        // Zero-pad a few frames at the start to give the encryption a
        // chance to sync
        tts_buffer.insert(tts_buffer.cend(), nframes * 6, 0.0);
        tts_buffer.insert(tts_buffer.cend(), tts_file.cbegin(), tts_file.cend());
    }

    static uint delay_periods = 0;
    static bool transmitting_prev = false;

    const bool mic_enabled = microphone_enabled(cfg);
    const bool transmitting_cur = mic_enabled || !tts_buffer.empty();
    if (transmitting_cur)
    {
        delay_periods = 0;

        // Only "prime" the resamplers on the "rising edge"
        if (!transmitting_prev)
        {
            input_resampler->enqueue_zeroes(nframes);
            input_resampler->clear();

            output_resampler->enqueue_zeroes(n_nom_modem_samples);
            output_resampler->clear();
        }

        // Turn on the PTT output
        set_ptt_val(cfg, true);

        const size_t tts_to_add = std::min(tts_buffer.size(), (size_t)nframes);
        if (tts_to_add > 0)
        {
            input_resampler->enqueue(tts_buffer.begin(),
                                     tts_buffer.begin() + tts_to_add);
            tts_buffer.erase(tts_buffer.begin(), tts_buffer.begin() + tts_to_add);
        }

        // Offset the voice samples so TTS doesn't add delay to the signal
        const jack_nframes_t voice_to_add = nframes - tts_to_add;
        // Only add voice if the mic is hot. Otherwise add zeroes
        if (mic_enabled)
        {
            input_resampler->enqueue(voice_frames + tts_to_add, voice_to_add);
        }
        else
        {
            input_resampler->enqueue_zeroes(voice_to_add);
        }

        // Now add the remaining frames without zero-padding
        while (input_resampler->available_elems() >= n_speech_samples)
        {
            short mod_out[n_nom_modem_samples];
            short voice_in[n_speech_samples];
            input_resampler->dequeue(voice_in, n_speech_samples);

            const size_t nout = crypto_tx->transmit(mod_out, voice_in);

            output_resampler->enqueue(mod_out, nout);
        }

        const uint modem_resampled_frames =
            get_nom_resampled_frames(n_nom_modem_samples,
                                     modem_sample_rate,
                                     jack_sample_rate);
        const uint required_frames =
            (modem_resampled_frames + (nframes - 1)) / nframes;
        const uint required_elems = nframes * required_frames;
        if (output_resampler->available_elems() >= required_elems)
        {
            output_resampler->dequeue(modem_frames, nframes);
        }
        else
        {
            zeroize_frames(modem_frames, nframes);
        }
    }
    else
    {
        // Only flush on the "falling edge"
        if (transmitting_prev)
        {
            // When the microphone is off we have to make sure we have flushed
            // all the voice and modem data out of the system and onto the modem
            // port.

            // Flush the input resampler to make sure all internal state is
            // written out. This will also reset the libsamplerate
            // state file
            input_resampler->flush(n_speech_samples * 2);

            // Run all the input data through the modem
            while (input_resampler->available_elems() != 0)
            {
                short mod_out[n_nom_modem_samples];
                // Initializing this buffer to zero will zero-fill the end
                // if there aren't a multiple of n_speech_samples in the
                // input queue
                short voice_in[n_speech_samples] = {0};
                input_resampler->dequeue(voice_in,
                                         std::min(n_speech_samples,
                                                  input_resampler->available_elems()));

                const size_t nout = crypto_tx->transmit(mod_out, voice_in);

                output_resampler->enqueue(mod_out, nout);
            }

            // Now that the output resampler has all the data it will, flush
            // it to make sure all internal state is written out. This will
            // also reset the libsamplerate state file
            output_resampler->flush(nframes * 2);
        }

        // Write out as much data to the modem port as we can. There may
        // be a few cycles' worth of data queued.
        const size_t available_frames =
            std::min((size_t)nframes, output_resampler->available_elems());
        const size_t remaining_frames = nframes - available_frames;
        output_resampler->dequeue(modem_frames, available_frames);
        if (remaining_frames > 0)
        {
            zeroize_frames(modem_frames + available_frames, remaining_frames);
        }

        // Force a new IV next time the microphone is active now that
        // the codec is idle
        crypto_tx->force_rekey_next_frame();

        // Once the buffer is empty turn off the PTT output after a delay
        if (available_frames == 0)
        {
            static const uint PTT_DEAD_KEY_PERIODS = 4;
            if (delay_periods == PTT_DEAD_KEY_PERIODS)
            {
                set_ptt_val(cfg, false);
            }
            else
            {
                ++delay_periods;
            }
        }
    }

    transmitting_prev = transmitting_cur;

    return 0;
}

static bool connect_input_ports(jack_port_t* output_port,
                                const char* input_port_regex)
{
    return connect_input_ports(client, output_port, input_port_regex);
}

static void activate_client()
{
    const struct config* cfg = crypto_tx->get_config();
    jack_nframes_t period = get_jack_period(cfg);
    char buffer[128] = {0};
    if (period == 0)
    {
        const jack_nframes_t jack_sample_rate = jack_get_sample_rate(client);
        const uint modem_sample_rate = crypto_tx->modem_sample_rate();
        const uint modem_samples_per_frame = crypto_tx->modem_samples_per_frame();

        period = get_nom_resampled_frames(modem_samples_per_frame,
                                          modem_sample_rate,
                                          jack_sample_rate);
        snprintf(buffer,
                 sizeof(buffer),
                 "Buffer size: %u, Modem frame size: %u, Modem sample rate: %u",
                 period,
                 modem_samples_per_frame,
                 modem_sample_rate);
    }
    else
    {
        snprintf(buffer, sizeof(buffer), "Buffer size: %u from config file", period);
    }

    crypto_tx->log_to_logger(LOG_INFO, buffer);
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
        *cfg->jack_voice_in_port ? cfg->jack_voice_in_port : "system:capture_1";
    if (jack_connect(client, capture_port_name, jack_port_name(voice_port)) != 0)
    {
        fprintf(stderr, "Could not connect modem port");
        exit (1);
    }

    const char* playback_port_regex =
        *cfg->jack_modem_out_port ? cfg->jack_modem_out_port : "system:playback_*";
    if (!connect_input_ports(modem_port, playback_port_regex))
    {
        exit(1);
    }
}

static void initialize_crypto()
{
    crypto_tx = nullptr;
    input_resampler = nullptr;
    output_resampler = nullptr;

    crypto_tx.reset(new crypto_tx_common("crypto_tx", config_file));

    const size_t speech_frames =
        get_nom_resampled_frames(crypto_tx->speech_samples_per_frame(),
                                 crypto_tx->speech_sample_rate(),
                                 jack_get_sample_rate(client));
    const size_t modem_frames =
        get_nom_resampled_frames(crypto_tx->modem_samples_per_frame(),
                                 crypto_tx->modem_sample_rate(),
                                 jack_get_sample_rate(client));

    input_resampler.reset(new resampler(SRC_SINC_FASTEST, 1, speech_frames * 2));
    output_resampler.reset(new resampler(SRC_SINC_FASTEST, 1, modem_frames * 2));
}

static void initialize_ptt()
{
    const struct config* cfg = crypto_tx->get_config();

    if (ptt_in_line != nullptr)
    {
        gpiod_line_close_chip(ptt_in_line);
        ptt_in_line = nullptr;
    }
    if (ptt_out_line != nullptr)
    {
        gpiod_line_close_chip(ptt_out_line);
        ptt_out_line = nullptr;
    }

    if (cfg->ptt_enabled)
    {
        if (cfg->ptt_gpio_num >= 0)
        {
            ptt_in_line = gpiod_line_get("gpiochip0", cfg->ptt_gpio_num);
            if (ptt_in_line != nullptr)
            {
                const int flags = cfg->ptt_gpio_bias | cfg->ptt_active_low;
                gpiod_line_request_input_flags(ptt_in_line, "jack_crypto_tx", flags);
            }
        }

        ptt_out_line = gpiod_line_get("gpiochip0", cfg->ptt_output_gpio_num);
        if (ptt_out_line != nullptr)
        {
            const int flags = cfg->ptt_output_bias |
                              cfg->ptt_output_drive |
                              cfg->ptt_output_active_low;
            gpiod_line_request_output_flags(ptt_out_line, "jack_crypto_tx", flags, 0);
        }
    }
}

int main(int argc, char *argv[])
{
    const char* client_name = "crypto_tx";
    const char* server_name = nullptr;
    jack_options_t options = JackNullOption;
    jack_status_t status;

    struct config *cur = NULL;

    if (argc > 2)
    {
        server_name = argv[1];
        options = (jack_options_t)(JackNullOption | JackServerName | JackNoStartServer);

        config_file = argv[2];
    }
    else
    {
        fprintf(stderr, "Usage: jack_crypto_tx <jack server name> <config file>");
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
                                    "voice_in",
                                    JACK_DEFAULT_AUDIO_TYPE,
                                    JackPortIsInput,
                                    0);

    modem_port = jack_port_register(client,
                                    "modem_out",
                                    JACK_DEFAULT_AUDIO_TYPE,
                                    JackPortIsOutput,
                                    0);

    if ((voice_port == NULL) || (modem_port == NULL))
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

    initialize_ptt();
    activate_client();

    signal(SIGQUIT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, handle_sighup);
    signal(SIGUSR1, handle_sigusr1);
    signal(SIGRTMIN, handle_sigptt);
    signal(SIGINT, signal_handler);

    // Create a zero length file to indicate when the transmitter is
    // initialized
    FILE* initialized = fopen("/var/run/tx_initialized", "w");
    if (initialized)
    {
        fclose(initialized);
    }


    const jack_nframes_t jack_sample_rate = jack_get_sample_rate(client);
    while (true)
    {
        if (reload_config != 0) {
            reload_config = 0;

            jack_deactivate(client);
            crypto_tx = nullptr;
            try
            {
                initialize_crypto();
            }
            catch (const std::exception& ex)
            {
                fprintf(stderr, "%s", ex.what());
                exit(1);
            }

            initialize_ptt();
            activate_client();
        }

        if (read_wav != 0)
        {
            read_wav = 0;

            if (read_wav_file("/tmp/tts.wav", jack_sample_rate, tts_file))
            {
                play_wav = 1;
            }
        }
        sleep(1);
    }
    
    jack_client_close (client);
    return 0;
}

