/*
 *  Copyright © 2017-2022 Wellington Wallace
 *
 *  This file is part of EasyEffects.
 *
 *  EasyEffects is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  EasyEffects is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with EasyEffects.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "rnnoise_ui.hpp"

namespace ui::rnnoise_box {

using namespace std::string_literals;

auto constexpr log_tag = "rnnoise_box: ";

static const std::string rnnn_ext = ".rnnn";

static const std::string default_model_name = _("Standard Model");

static std::filesystem::path model_dir = g_get_user_config_dir() + "/easyeffects/rnnoise"s;

struct Data {
 public:
  ~Data() { util::debug(log_tag + "data struct destroyed"s); }

  app::Application* application;

  std::shared_ptr<RNNoise> rnnoise;

  std::vector<sigc::connection> connections;

  std::vector<gulong> gconnections;
};

struct _RNNoiseBox {
  GtkBox parent_instance;

  GtkScale *input_gain, *output_gain;

  GtkLevelBar *input_level_left, *input_level_right, *output_level_left, *output_level_right;

  GtkLabel *input_level_left_label, *input_level_right_label, *output_level_left_label, *output_level_right_label;

  GtkToggleButton* bypass;

  GtkListView* listview;

  GtkStringList* string_list;

  GtkSingleSelection* selection_model;

  GSettings* settings;

  GFileMonitor* folder_monitor;

  Data* data;
};

G_DEFINE_TYPE(RNNoiseBox, rnnoise_box, GTK_TYPE_BOX)

void on_bypass(RNNoiseBox* self, GtkToggleButton* btn) {
  self->data->rnnoise->bypass = gtk_toggle_button_get_active(btn);
}

void on_reset(RNNoiseBox* self, GtkButton* btn) {
  gtk_toggle_button_set_active(self->bypass, 0);

  util::reset_all_keys(self->settings);
}

gboolean set_model_delete_button_visibility(GtkListItem* item, const char* name) {
  if (name == default_model_name) {
    return 0;
  }

  return 1;
}

void on_remove_model_file(GtkListItem* item, GtkButton* btn) {
  std::string name = gtk_string_object_get_string(GTK_STRING_OBJECT(gtk_list_item_get_item(item)));

  const auto model_file = model_dir / std::filesystem::path{name.c_str() + rnnn_ext};

  if (std::filesystem::exists(model_file)) {
    std::filesystem::remove(model_file);

    util::debug(log_tag + "removed model file: "s + model_file.string());
  }
}

void import_model_file(const std::string& file_path) {
  std::filesystem::path p{file_path};

  if (std::filesystem::is_regular_file(p)) {
    auto out_path = model_dir / p.filename();

    out_path.replace_extension(rnnn_ext);

    std::filesystem::copy_file(p, out_path, std::filesystem::copy_options::overwrite_existing);

    util::debug(log_tag + "imported model file to: "s + out_path.string());
  } else {
    util::warning(log_tag + p.string() + " is not a file!");
  }
}

void on_import_model_clicked(RNNoiseBox* self, GtkButton* btn) {
  auto* active_window = gtk_application_get_active_window(GTK_APPLICATION(self->data->application));

  auto* dialog = gtk_file_chooser_native_new(_("Import Model File"), active_window, GTK_FILE_CHOOSER_ACTION_OPEN,
                                             _("Open"), _("Cancel"));

  auto* filter = gtk_file_filter_new();

  gtk_file_filter_set_name(filter, _("RNNoise Models"));
  gtk_file_filter_add_pattern(filter, "*.rnnn");

  gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(dialog), filter);

  g_signal_connect(dialog, "response", G_CALLBACK(+[](GtkNativeDialog* dialog, int response, RNNoiseBox* self) {
                     if (response == GTK_RESPONSE_ACCEPT) {
                       auto* chooser = GTK_FILE_CHOOSER(dialog);
                       auto* file = gtk_file_chooser_get_file(chooser);
                       auto* path = g_file_get_path(file);

                       import_model_file(path);

                       g_object_unref(file);
                     }

                     g_object_unref(dialog);
                   }),
                   self);

  gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(dialog), 1);
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}

void setup_listview(RNNoiseBox* self) {
  for (const auto& name : util::get_files_name(model_dir, rnnn_ext)) {
    gtk_string_list_append(self->string_list, name.c_str());
  }

  if (g_list_model_get_n_items(G_LIST_MODEL(self->string_list)) == 0) {
    g_settings_set_string(self->settings, "model-path", "");
  }
}

void setup(RNNoiseBox* self,
           std::shared_ptr<RNNoise> rnnoise,
           const std::string& schema_path,
           app::Application* application) {
  self->data->rnnoise = rnnoise;
  self->data->application = application;

  self->settings = g_settings_new_with_path("com.github.wwmm.easyeffects.rnnoise", schema_path.c_str());

  rnnoise->post_messages = true;
  rnnoise->bypass = false;

  setup_listview(self);

  self->data->connections.push_back(rnnoise->input_level.connect([=](const float& left, const float& right) {
    update_level(self->input_level_left, self->input_level_left_label, self->input_level_right,
                 self->input_level_right_label, left, right);
  }));

  self->data->connections.push_back(rnnoise->output_level.connect([=](const float& left, const float& right) {
    update_level(self->output_level_left, self->output_level_left_label, self->output_level_right,
                 self->output_level_right_label, left, right);
  }));

  gsettings_bind_widgets<"input-gain", "output-gain">(self->settings, self->input_gain, self->output_gain);

  g_settings_bind_with_mapping(
      self->settings, "model-path", self->selection_model, "selected", G_SETTINGS_BIND_DEFAULT,
      +[](GValue* value, GVariant* variant, gpointer user_data) {
        auto self = EE_RNNOISE_BOX(user_data);

        const auto* v = g_variant_get_string(variant, nullptr);

        auto path = std::filesystem::path{v};

        auto gsettings_model_name = path.stem();

        int standard_model_id = 0;

        for (guint n = 0; n < g_list_model_get_n_items(G_LIST_MODEL(self->selection_model)); n++) {
          auto item = g_list_model_get_item(G_LIST_MODEL(self->selection_model), n);

          std::string model_name = gtk_string_object_get_string(GTK_STRING_OBJECT(item));

          g_object_unref(item);

          if (gsettings_model_name == model_name) {
            g_value_set_uint(value, n);
          } else if (model_name == default_model_name) {
            standard_model_id = n;
          }
        }

        if (gsettings_model_name.empty()) {
          g_value_set_uint(value, standard_model_id);
        }

        return 1;
      },
      +[](const GValue* value, const GVariantType* expected_type, gpointer user_data) {
        auto self = EE_RNNOISE_BOX(user_data);

        auto string_object =
            GTK_STRING_OBJECT(gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(self->selection_model)));

        std::string name = gtk_string_object_get_string(string_object);

        if (name == default_model_name) {
          return g_variant_new_string("");
        }

        const auto model_file = model_dir / std::filesystem::path{name + rnnn_ext};

        return g_variant_new_string(model_file.c_str());
      },
      self, nullptr);
}

void dispose(GObject* object) {
  auto* self = EE_RNNOISE_BOX(object);

  self->data->rnnoise->bypass = false;

  g_file_monitor_cancel(self->folder_monitor);

  g_object_unref(self->folder_monitor);

  for (auto& c : self->data->connections) {
    c.disconnect();
  }

  for (auto& handler_id : self->data->gconnections) {
    g_signal_handler_disconnect(self->settings, handler_id);
  }

  self->data->connections.clear();
  self->data->gconnections.clear();

  g_object_unref(self->settings);

  util::debug(log_tag + "disposed"s);

  G_OBJECT_CLASS(rnnoise_box_parent_class)->dispose(object);
}

void finalize(GObject* object) {
  auto* self = EE_RNNOISE_BOX(object);

  delete self->data;

  util::debug(log_tag + "finalized"s);

  G_OBJECT_CLASS(rnnoise_box_parent_class)->finalize(object);
}

void rnnoise_box_class_init(RNNoiseBoxClass* klass) {
  auto* object_class = G_OBJECT_CLASS(klass);
  auto* widget_class = GTK_WIDGET_CLASS(klass);

  object_class->dispose = dispose;
  object_class->finalize = finalize;

  gtk_widget_class_set_template_from_resource(widget_class, "/com/github/wwmm/easyeffects/ui/rnnoise.ui");

  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, input_gain);
  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, output_gain);
  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, input_level_left);
  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, input_level_right);
  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, output_level_left);
  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, output_level_right);
  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, input_level_left_label);
  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, input_level_right_label);
  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, output_level_left_label);
  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, output_level_right_label);

  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, bypass);

  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, string_list);
  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, selection_model);
  gtk_widget_class_bind_template_child(widget_class, RNNoiseBox, listview);

  gtk_widget_class_bind_template_callback(widget_class, on_bypass);
  gtk_widget_class_bind_template_callback(widget_class, on_reset);
  gtk_widget_class_bind_template_callback(widget_class, on_import_model_clicked);
  gtk_widget_class_bind_template_callback(widget_class, on_remove_model_file);
  gtk_widget_class_bind_template_callback(widget_class, set_model_delete_button_visibility);
}

void rnnoise_box_init(RNNoiseBox* self) {
  gtk_widget_init_template(GTK_WIDGET(self));

  self->data = new Data();

  prepare_scales<"dB">(self->input_gain, self->output_gain);

  // model dir

  if (!std::filesystem::is_directory(model_dir)) {
    if (std::filesystem::create_directories(model_dir)) {
      util::debug(log_tag + "model directory created: "s + model_dir.string());
    } else {
      util::warning(log_tag + "failed to create model directory: "s + model_dir.string());
    }
  } else {
    util::debug(log_tag + "model directory already exists: "s + model_dir.string());
  }

  auto gfile = g_file_new_for_path(model_dir.c_str());

  self->folder_monitor = g_file_monitor_directory(gfile, G_FILE_MONITOR_NONE, nullptr, nullptr);

  g_signal_connect(self->folder_monitor, "changed",
                   G_CALLBACK(+[](GFileMonitor* monitor, GFile* file, GFile* other_file, GFileMonitorEvent event_type,
                                  RNNoiseBox* self) {
                     const auto rnn_filename = util::remove_filename_extension(g_file_get_basename(file));

                     if (rnn_filename.empty()) {
                       util::warning(log_tag + "can't retrieve information about the rnn file"s);

                       return;
                     }

                     switch (event_type) {
                       case G_FILE_MONITOR_EVENT_CREATED: {
                         for (guint n = 0; n < g_list_model_get_n_items(G_LIST_MODEL(self->string_list)); n++) {
                           if (rnn_filename == gtk_string_list_get_string(self->string_list, n)) {
                             return;
                           }
                         }

                         gtk_string_list_append(self->string_list, rnn_filename.c_str());

                         break;
                       }

                       case G_FILE_MONITOR_EVENT_DELETED: {
                         for (guint n = 0; n < g_list_model_get_n_items(G_LIST_MODEL(self->string_list)); n++) {
                           if (rnn_filename == gtk_string_list_get_string(self->string_list, n)) {
                             gtk_string_list_remove(self->string_list, n);

                             // Workaround for GTK not calling the listview signal_selection_changed (issue #1110)

                             //  on_selection_changed();

                             return;
                           }
                         }

                         break;
                       }

                       default:
                         break;
                     }
                   }),
                   self);

  g_object_unref(gfile);
}

auto create() -> RNNoiseBox* {
  return static_cast<RNNoiseBox*>(g_object_new(EE_TYPE_RNNOISE_BOX, nullptr));
}

}  // namespace ui::rnnoise_box
