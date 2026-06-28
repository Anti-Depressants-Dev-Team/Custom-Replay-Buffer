#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-hotkey.h>
#include <util/dstr.h>
#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include "plugin-support.h"

// Qt Headers
#include <QDockWidget>
#include <QWidget>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QSpinBox>
#include <QPushButton>
#include <QMainWindow>
#include <QLabel>
#include <QMetaObject>
#include <QKeyEvent>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("multiple-replay-buffers", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Multiple Replay Buffers plugin";
}

#define NUM_HOTKEYS 6
int durations[NUM_HOTKEYS] = { 15, 30, 60, 120, 180, 300 };
obs_hotkey_id hotkey_ids[NUM_HOTKEYS];
std::vector<int> pending_cuts;
std::mutex cuts_mutex;

// Qt Dock Variables
QSpinBox *spinboxes[NUM_HOTKEYS] = { nullptr };

std::string get_ffmpeg_dir() {
    char *appdata = getenv("APPDATA");
    if (!appdata) return "";
    std::string path = std::string(appdata) + "\\obs-studio\\plugin_config\\multiple-replay-buffers";
    CreateDirectoryA(path.c_str(), NULL);
    return path;
}

std::string get_ffmpeg_exe() {
    return get_ffmpeg_dir() + "\\ffmpeg\\bin\\ffmpeg.exe";
}

void download_ffmpeg_if_missing() {
    std::string exe_path = get_ffmpeg_exe();
    if (GetFileAttributesA(exe_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return; // Already exists
    }

    std::thread([]() {
        obs_log(LOG_INFO, "[Multiple Replay Buffers] FFmpeg not found. Downloading via PowerShell...");
        std::string dir = get_ffmpeg_dir();
        std::string zip_path = dir + "\\ffmpeg.zip";
        
        std::string ps_cmd = "powershell -Command \"";
        ps_cmd += "Invoke-WebRequest -Uri 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip' -OutFile '" + zip_path + "'; ";
        ps_cmd += "Expand-Archive -Path '" + zip_path + "' -DestinationPath '" + dir + "' -Force; ";
        ps_cmd += "Move-Item -Path '" + dir + "\\ffmpeg-master-latest-win64-gpl' -Destination '" + dir + "\\ffmpeg' -Force; ";
        ps_cmd += "Remove-Item -Path '" + zip_path + "' -Force\"";

        STARTUPINFOA si = { sizeof(STARTUPINFOA) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi;

        char cmd[2048];
        strcpy_s(cmd, ps_cmd.c_str());

        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            obs_log(LOG_INFO, "[Multiple Replay Buffers] FFmpeg download and extraction complete.");
        } else {
            obs_log(LOG_ERROR, "[Multiple Replay Buffers] Failed to launch PowerShell to download FFmpeg.");
        }
    }).detach();
}

void process_cuts(std::string input_path, std::vector<int> durations_to_cut) {
    std::string exe_path = get_ffmpeg_exe();
    if (GetFileAttributesA(exe_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        obs_log(LOG_WARNING, "[Multiple Replay Buffers] FFmpeg is missing. Cannot trim video yet.");
        return;
    }

    std::thread([exe_path, input_path, durations_to_cut]() {
        size_t dot_idx = input_path.find_last_of(".");
        if (dot_idx == std::string::npos) return;

        std::string ext = input_path.substr(dot_idx);
        std::string base = input_path.substr(0, dot_idx);

        for (int duration : durations_to_cut) {
            std::string output_path = base + "_" + std::to_string(duration) + "s" + ext;
            obs_log(LOG_INFO, "[Multiple Replay Buffers] Trimming video to %d seconds...", duration);

            std::string cmd = "\"" + exe_path + "\" -y -sseof -" + std::to_string(duration) + " -i \"" + input_path + "\" -map 0 -c copy \"" + output_path + "\"";
            
            STARTUPINFOA si = { sizeof(STARTUPINFOA) };
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi;

            char cmd_buf[2048];
            strcpy_s(cmd_buf, cmd.c_str());

            if (CreateProcessA(NULL, cmd_buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, INFINITE);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                obs_log(LOG_INFO, "[Multiple Replay Buffers] Successfully saved custom replay: %s", output_path.c_str());
            } else {
                obs_log(LOG_ERROR, "[Multiple Replay Buffers] Failed to launch FFmpeg process.");
            }
        }
        
        // After trimming, delete the original base replay file
        DeleteFileA(input_path.c_str());
        obs_log(LOG_INFO, "[Multiple Replay Buffers] Deleted original base replay file: %s", input_path.c_str());

    }).detach();
}

void save_load_callback(obs_data_t *save_data, bool saving, void *private_data) {
    if (saving) {
        // Save to OBS config
        obs_data_t *durations_obj = obs_data_create();
        for (int i = 0; i < NUM_HOTKEYS; i++) {
            obs_data_set_int(durations_obj, ("duration_" + std::to_string(i)).c_str(), durations[i]);
            
            obs_data_array_t *hk_array = obs_hotkey_save(hotkey_ids[i]);
            obs_data_set_array(durations_obj, ("hotkey_" + std::to_string(i)).c_str(), hk_array);
            obs_data_array_release(hk_array);
        }
        obs_data_set_obj(save_data, "multiple_replay_buffers", durations_obj);
        obs_data_release(durations_obj);
    } else {
        // Load from OBS config
        obs_data_t *durations_obj = obs_data_get_obj(save_data, "multiple_replay_buffers");
        if (durations_obj) {
            for (int i = 0; i < NUM_HOTKEYS; i++) {
                int val = obs_data_get_int(durations_obj, ("duration_" + std::to_string(i)).c_str());
                if (val > 0) durations[i] = val;
                
                obs_data_array_t *hk_array = obs_data_get_array(durations_obj, ("hotkey_" + std::to_string(i)).c_str());
                if (hk_array) {
                    obs_hotkey_load(hotkey_ids[i], hk_array);
                    obs_data_array_release(hk_array);
                }
            }
            obs_data_release(durations_obj);
            
            // Sync Qt UI if it has been created
            if (spinboxes[0]) {
                for (int i = 0; i < NUM_HOTKEYS; i++) {
                    if (spinboxes[i]) {
                        QMetaObject::invokeMethod(spinboxes[i], "setValue", Qt::QueuedConnection, Q_ARG(int, durations[i]));
                    }
                }
            }
        }
    }
}

class HotkeyButton : public QPushButton {
public:
    int hotkey_index;
    
    HotkeyButton(int index) : QPushButton(""), hotkey_index(index) {
        setCheckable(true);
    }
    
protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (!isChecked()) {
            QPushButton::keyPressEvent(event);
            return;
        }
        
        event->accept();
        
        uint32_t modifiers = 0;
        if (event->modifiers() & Qt::ShiftModifier) modifiers |= INTERACT_SHIFT_KEY;
        if (event->modifiers() & Qt::ControlModifier) modifiers |= INTERACT_CONTROL_KEY;
        if (event->modifiers() & Qt::AltModifier) modifiers |= INTERACT_ALT_KEY;
        if (event->modifiers() & Qt::MetaModifier) modifiers |= INTERACT_COMMAND_KEY;
        
        int vk = event->nativeVirtualKey();
        obs_key_t key = obs_key_from_virtual_key(vk);
        
        // Ignore standalone modifier keys to wait for actual key
        if (key == OBS_KEY_SHIFT || key == OBS_KEY_CONTROL || key == OBS_KEY_ALT || key == OBS_KEY_META) {
            return;
        }

        if (key != OBS_KEY_NONE) {
            obs_key_combination_t combo;
            combo.modifiers = modifiers;
            combo.key = key;
            
            obs_hotkey_load_bindings(hotkey_ids[hotkey_index], &combo, 1);
            
            setChecked(false);
            update_text();
        } else if (event->key() == Qt::Key_Escape) {
            setChecked(false);
            update_text();
        } else if (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) {
            obs_key_combination_t combo = {0, OBS_KEY_NONE};
            obs_hotkey_load_bindings(hotkey_ids[hotkey_index], &combo, 1);
            setChecked(false);
            update_text();
        }
    }
    
    void nextCheckState() override {
        QPushButton::nextCheckState();
        update_text();
    }

public:
    void update_text() {
        if (isChecked()) {
            setText("Press a key (Esc/Del)...");
        } else {
            obs_key_combination_t combo = {0, OBS_KEY_NONE};
            obs_data_array_t *array = obs_hotkey_save(hotkey_ids[hotkey_index]);
            if (array && obs_data_array_count(array) > 0) {
                obs_data_t *item = obs_data_array_item(array, 0);
                combo.key = obs_key_from_name(obs_data_get_string(item, "key"));
                combo.modifiers = 0;
                if (obs_data_get_bool(item, "shift")) combo.modifiers |= INTERACT_SHIFT_KEY;
                if (obs_data_get_bool(item, "control")) combo.modifiers |= INTERACT_CONTROL_KEY;
                if (obs_data_get_bool(item, "alt")) combo.modifiers |= INTERACT_ALT_KEY;
                if (obs_data_get_bool(item, "command")) combo.modifiers |= INTERACT_COMMAND_KEY;
                obs_data_release(item);
            }
            if (array) obs_data_array_release(array);

            if (combo.key == OBS_KEY_NONE) {
                setText("Not Bound");
            } else {
                struct dstr str = {0};
                obs_key_combination_to_str(combo, &str);
                setText(str.array ? str.array : "Bound");
                dstr_free(&str);
            }
        }
    }
};

void setup_dock() {
    QWidget *container = new QWidget();
    QFormLayout *layout = new QFormLayout();
    
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        spinboxes[i] = new QSpinBox();
        spinboxes[i]->setRange(1, 3600);
        spinboxes[i]->setSuffix(" s");
        spinboxes[i]->setValue(durations[i]);

        HotkeyButton *hk_btn = new HotkeyButton(i);
        hk_btn->update_text();
        
        QHBoxLayout *row_layout = new QHBoxLayout();
        row_layout->addWidget(spinboxes[i]);
        row_layout->addWidget(hk_btn);
        
        QString label = QString("Hotkey %1 Length:").arg(i+1);
        layout->addRow(label, row_layout);
    }
    
    QPushButton *apply_btn = new QPushButton("Apply & Save");
    QObject::connect(apply_btn, &QPushButton::clicked, [=]() {
        for (int i = 0; i < NUM_HOTKEYS; i++) {
            durations[i] = spinboxes[i]->value();
        }
        obs_frontend_save(); // Force OBS to save profile settings immediately
    });
    layout->addRow(apply_btn);
    
    container->setLayout(layout);
    
    obs_frontend_add_dock_by_id("multipleReplayBuffersDock", "Custom Replay Buffers", container);
}

static void on_frontend_event(enum obs_frontend_event event, void *private_data) {
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
        setup_dock();
    }
    else if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED) {
        std::vector<int> cuts_to_process;
        {
            std::lock_guard<std::mutex> lock(cuts_mutex);
            if (pending_cuts.empty()) return;
            cuts_to_process = pending_cuts;
            pending_cuts.clear();
        }

        char *last_replay = obs_frontend_get_last_replay();
        if (!last_replay) {
            obs_log(LOG_ERROR, "[Multiple Replay Buffers] Could not get last replay path.");
            return;
        }

        std::string replay_path(last_replay);
        bfree(last_replay);

        process_cuts(replay_path, cuts_to_process);
    }
}

static void hotkey_callback(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed) {
    if (!pressed) return;
    
    int idx = (int)(intptr_t)data;
    if (idx < 0 || idx >= NUM_HOTKEYS) return;

    int dur = durations[idx];
    obs_log(LOG_INFO, "[Multiple Replay Buffers] Custom replay requested: %d seconds", dur);
    
    {
        std::lock_guard<std::mutex> lock(cuts_mutex);
        pending_cuts.push_back(dur);
    }
    
    obs_frontend_replay_buffer_save();
}

bool obs_module_load(void)
{
    obs_log(LOG_INFO, "Loading Multiple Replay Buffers Plugin");
    
    download_ffmpeg_if_missing();

    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    obs_frontend_add_save_callback(save_load_callback, nullptr);

    // Make names permanent strings to satisfy OBS internal references
    static char names[NUM_HOTKEYS][64];
    static char descs[NUM_HOTKEYS][128];

    for (int i = 0; i < NUM_HOTKEYS; i++) {
        snprintf(names[i], sizeof(names[i]), "multiple_replay_buffers_%d", i);
        snprintf(descs[i], sizeof(descs[i]), "Save Replay: Custom Length %d", i + 1);
        hotkey_ids[i] = obs_hotkey_register_frontend(names[i], descs[i], hotkey_callback, (void*)(intptr_t)i);
    }

    return true;
}

void obs_module_unload(void)
{
    obs_frontend_remove_event_callback(on_frontend_event, nullptr);
    obs_frontend_remove_save_callback(save_load_callback, nullptr);
    obs_log(LOG_INFO, "Multiple Replay Buffers Plugin unloaded");
}
