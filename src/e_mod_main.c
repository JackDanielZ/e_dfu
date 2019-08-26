#define EFL_BETA_API_SUPPORT
#define EFL_EO_API_SUPPORT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <syslog.h>

#include <e.h>
#include <Eina.h>
#include <Ecore.h>
#include <Ecore_Con.h>
#include <Eeze.h>

#include "e_mod_main.h"

#define _EET_ENTRY "config"

typedef struct
{
   E_Gadcon_Client *gcc;
   E_Gadcon_Popup *popup;
   Evas_Object *o_icon;
   Eo *main_box;

   Ecore_File_Monitor *config_file_monitor;
} Instance;

#define PRINT _printf

static E_Module *_module = NULL;

typedef struct
{
   Eina_Stringshare *name;
   Eina_Stringshare *command;
} Image_Info;

typedef struct
{
   Eina_Stringshare *name;
   Eina_Stringshare *id;
   Eina_Stringshare *default_image;
   Eina_List *images; /* List of Image_Info */
} Device_Info;

typedef struct
{
   Eina_List *devices; /* List of Device_Info */
} Config;

static Config *_config = NULL;
static Eet_Data_Descriptor *_config_edd = NULL;

static int
_printf(const char *fmt, ...)
{
   static FILE *fp = NULL;
   char printf_buf[1024];
   va_list args;
   int printed;

   if (!fp)
     {
        char path[1024];
        sprintf(path, "%s/e_dfu/log", efreet_config_home_get());
        fp = fopen(path, "a");
     }

   va_start(args, fmt);
   printed = vsprintf(printf_buf, fmt, args);
   va_end(args);

   fwrite(printf_buf, 1, strlen(printf_buf), fp);
   fflush(fp);

   return printed;
}

static void
_config_eet_load()
{
   Eet_Data_Descriptor *device_edd, *image_edd;
   if (_config_edd) return;
   Eet_Data_Descriptor_Class eddc;

   EET_EINA_STREAM_DATA_DESCRIPTOR_CLASS_SET(&eddc, Image_Info);
   image_edd = eet_data_descriptor_stream_new(&eddc);
   EET_DATA_DESCRIPTOR_ADD_BASIC(image_edd, Image_Info, "name", name, EET_T_STRING);
   EET_DATA_DESCRIPTOR_ADD_BASIC(image_edd, Image_Info, "command", command, EET_T_STRING);

   EET_EINA_STREAM_DATA_DESCRIPTOR_CLASS_SET(&eddc, Device_Info);
   device_edd = eet_data_descriptor_stream_new(&eddc);
   EET_DATA_DESCRIPTOR_ADD_BASIC(device_edd, Device_Info, "name", name, EET_T_STRING);
   EET_DATA_DESCRIPTOR_ADD_BASIC(device_edd, Device_Info, "id", id, EET_T_STRING);
   EET_DATA_DESCRIPTOR_ADD_BASIC(device_edd, Device_Info, "default_image", default_image, EET_T_STRING);
   EET_DATA_DESCRIPTOR_ADD_LIST(device_edd, Device_Info, "images", images, image_edd);

   EET_EINA_STREAM_DATA_DESCRIPTOR_CLASS_SET(&eddc, Config);
   _config_edd = eet_data_descriptor_stream_new(&eddc);
   EET_DATA_DESCRIPTOR_ADD_LIST(_config_edd, Config, "devices", devices, device_edd);
}

static void
_config_save()
{
   char path[1024];
   sprintf(path, "%s/e_dfu/config", efreet_config_home_get());
   _config_eet_load();
   Eet_File *file = eet_open(path, EET_FILE_MODE_WRITE);
   eet_data_write(file, _config_edd, _EET_ENTRY, _config, EINA_TRUE);
   eet_close(file);
}

static Eina_Bool
_mkdir(const char *dir)
{
   if (!ecore_file_exists(dir))
     {
        Eina_Bool success = ecore_file_mkdir(dir);
        if (!success)
          {
             PRINT("Cannot create a config folder \"%s\"\n", dir);
             return EINA_FALSE;
          }
     }
   return EINA_TRUE;
}

static void
_config_init()
{
   char path[1024];

   sprintf(path, "%s/e_dfu", efreet_config_home_get());
   if (!_mkdir(path)) return;

   _config_eet_load();
   sprintf(path, "%s/e_dfu/config", efreet_config_home_get());
   Eet_File *file = eet_open(path, EET_FILE_MODE_READ);
   if (!file)
     {
        PRINT("DFU new config\n");
        Device_Info *dev = calloc(1, sizeof(*dev));
        Image_Info *img = calloc(1, sizeof(*img));
        Image_Info *img2 = calloc(1, sizeof(*img2));

        dev->name = eina_stringshare_add("Example");
        dev->id = eina_stringshare_add("1234:5678");
        dev->default_image = eina_stringshare_add("target");

        img->name = eina_stringshare_add("target");
        img->command = eina_stringshare_add("echo \"Target launched\"");
        dev->images = eina_list_append(dev->images, img);

        img2->name = eina_stringshare_add("target2");
        img2->command = eina_stringshare_add("echo \"Target2 launched\"");
        dev->images = eina_list_append(dev->images, img2);

        _config = calloc(1, sizeof(Config));
        _config->devices = eina_list_append(_config->devices, dev);
        _config_save();
     }
   else
     {
        _config = eet_data_read(file, _config_edd, _EET_ENTRY);
        eet_close(file);
     }
}

static void
_config_shutdown()
{
   Device_Info *dev;
   EINA_LIST_FREE(_config->devices, dev)
     {
        Image_Info *img;
        eina_stringshare_del(dev->name);
        eina_stringshare_del(dev->id);
        eina_stringshare_del(dev->default_image);
        EINA_LIST_FREE(dev->images, img)
          {
             eina_stringshare_del(img->name);
             eina_stringshare_del(img->command);
             free(img);
          }
        free(dev);
     }
   free(_config);
   _config = NULL;
}

static Eina_Stringshare *
_device_id_get(const char *device)
{
   char *path = strdup(device);
   char buf[1024];
   while (*path)
     {
        char vendor[4], product[4], *slash;
        *vendor = '\0';
        *product = '\0';
        sprintf(buf, "%s/idVendor", path);
        if (access(buf, R_OK) == 0)
          {
             FILE* fp = fopen(buf, "r");
             fread(vendor, 1, sizeof(vendor), fp);
             fclose(fp);
          }
        sprintf(buf, "%s/idProduct", path);
        if (access(buf, R_OK) == 0)
          {
             FILE* fp = fopen(buf, "r");
             fread(product, 1, sizeof(product), fp);
             fclose(fp);
          }
        if (*vendor && *product)
          {
             return eina_stringshare_printf("%.4s:%.4s", vendor, product);
          }
        slash = strrchr(path, '/');
        if (slash) *slash = '\0';
     }
   return NULL;
}

static void
_udev_added_cb(const char *device, Eeze_Udev_Event  event EINA_UNUSED,
             void *data EINA_UNUSED, Eeze_Udev_Watch *watch EINA_UNUSED)
{
   PRINT("Device added %s\n", device);
   PRINT("ID: %s\n", _device_id_get(device));
}

static Eo *
_label_create(Eo *parent, const char *text, Eo **wref)
{
   Eo *label = wref ? *wref : NULL;
   if (!label)
     {
        label = elm_label_add(parent);
        evas_object_size_hint_align_set(label, EVAS_HINT_FILL, EVAS_HINT_FILL);
        evas_object_size_hint_weight_set(label, 0.0, 0.0);
        evas_object_show(label);
        if (wref) efl_wref_add(label, wref);
     }
   elm_object_text_set(label, text);
   return label;
}

static Eo *
_button_create(Eo *parent, const char *text, Eo *icon, Eo **wref, Evas_Smart_Cb cb_func, void *cb_data)
{
   Eo *bt = wref ? *wref : NULL;
   if (!bt)
     {
        bt = elm_button_add(parent);
        evas_object_size_hint_align_set(bt, EVAS_HINT_FILL, EVAS_HINT_FILL);
        evas_object_size_hint_weight_set(bt, 0.0, 0.0);
        evas_object_show(bt);
        if (wref) efl_wref_add(bt, wref);
        if (cb_func) evas_object_smart_callback_add(bt, "clicked", cb_func, cb_data);
     }
   elm_object_text_set(bt, text);
   elm_object_part_content_set(bt, "icon", icon);
   return bt;
}

static void
_image_selected(void *data, Evas_Object *bt, void *event_info EINA_UNUSED)
{
   Device_Info *dev = data;
   Eo *hv = efl_key_data_get(bt, "hover");
   eina_stringshare_del(dev->default_image);
   if (!strcmp(elm_object_text_get(bt), "Disabled"))
      dev->default_image = NULL;
   else
      dev->default_image = eina_stringshare_add(elm_object_text_get(bt));
   _config_save();
   efl_del(hv);
}

static void
_images_bt_clicked(void *data, Evas_Object *obj, void *event_info EINA_UNUSED)
{
   Eina_List *itr;
   Image_Info *img;
   Device_Info *dev = data;
   Instance *inst = efl_key_data_get(obj, "Instance");

   Eo *hv = elm_hover_add(inst->main_box), *bt;
   evas_object_layer_set(hv, E_LAYER_MENU);
   elm_hover_parent_set(hv, inst->main_box);
   elm_hover_target_set(hv, obj);
   efl_gfx_entity_visible_set(hv, EINA_TRUE);
   Eo *bx = elm_box_add(hv);
   elm_box_homogeneous_set(bx, EINA_TRUE);
   EINA_LIST_FOREACH(dev->images, itr, img)
     {
        bt = _button_create(bx, img->name, NULL, NULL, _image_selected, dev);
        efl_key_data_set(bt, "hover", hv);
        elm_box_pack_end(bx, bt);
     }
   bt = _button_create(bx, "Disabled", NULL, NULL, _image_selected, dev);
   efl_key_data_set(bt, "hover", hv);
   elm_box_pack_end(bx, bt);

   evas_object_show(bx);
   elm_object_part_content_set(hv, "top", bx);
}

static void
_box_update(Instance *inst, Eina_Bool clear)
{
   Eina_List *itr;
   Device_Info *dev;

   if (!inst->main_box) return;

   if (clear) elm_box_clear(inst->main_box);

   EINA_LIST_FOREACH(_config->devices, itr, dev)
     {
        char lb_text[256];
        Eo *b = elm_box_add(inst->main_box), *o;
        elm_box_horizontal_set(b, EINA_TRUE);
        evas_object_size_hint_align_set(b, EVAS_HINT_FILL, EVAS_HINT_FILL);
        evas_object_size_hint_weight_set(b, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
        evas_object_show(b);
        elm_box_pack_end(inst->main_box, b);

        sprintf(lb_text, "%s (%s) ", dev->name, dev->id);
        elm_box_pack_end(b, _label_create(b, lb_text, NULL));

        o = _button_create(b, dev->default_image ? dev->default_image : "Disabled",
              NULL, NULL, _images_bt_clicked, dev);
        efl_key_data_set(o, "Instance", inst);
        elm_box_pack_end(b, o);
     }
}

static void
_config_file_changed(void *data,
      Ecore_File_Monitor *em EINA_UNUSED,
      Ecore_File_Event event, const char *_path EINA_UNUSED)
{
   Instance *inst = data;
   if (event != ECORE_FILE_EVENT_MODIFIED) return;
   PRINT("Config updated\n");
   _config_shutdown();
   E_FREE_FUNC(inst->popup, e_object_del);
   _config_init();
}

static Instance *
_instance_create()
{
   char path[1024];
   Instance *inst = calloc(1, sizeof(Instance));

   sprintf(path, "%s/e_dfu", efreet_config_home_get());
   if (!_mkdir(path)) return NULL;
   sprintf(path, "%s/e_dfu/config", efreet_config_home_get());
   inst->config_file_monitor = ecore_file_monitor_add(path, _config_file_changed, inst);

   return inst;
}

static void
_instance_delete(Instance *inst)
{
   if (inst->o_icon) evas_object_del(inst->o_icon);
   if (inst->main_box) evas_object_del(inst->main_box);

   free(inst);
}

static void
_popup_del(Instance *inst)
{
   E_FREE_FUNC(inst->popup, e_object_del);
}

static void
_popup_del_cb(void *obj)
{
   _popup_del(e_object_data_get(obj));
}

static void
_popup_comp_del_cb(void *data, Evas_Object *obj EINA_UNUSED)
{
   Instance *inst = data;

   E_FREE_FUNC(inst->popup, e_object_del);
}

static void
_button_cb_mouse_down(void *data, Evas *e EINA_UNUSED, Evas_Object *obj EINA_UNUSED, void *event_info)
{
   Instance *inst;
   Evas_Event_Mouse_Down *ev;

   inst = data;
   ev = event_info;
   if (ev->button == 1)
     {
        if (!inst->popup)
          {
             Evas_Object *o;
             inst->popup = e_gadcon_popup_new(inst->gcc, 0);

             o = elm_box_add(e_comp->elm);
             evas_object_size_hint_align_set(o, EVAS_HINT_FILL, EVAS_HINT_FILL);
             evas_object_size_hint_weight_set(o, EVAS_HINT_EXPAND, 0.0);
             evas_object_show(o);
             efl_wref_add(o, &inst->main_box);

             _box_update(inst, EINA_FALSE);

             e_gadcon_popup_content_set(inst->popup, inst->main_box);
             e_comp_object_util_autoclose(inst->popup->comp_object,
                   _popup_comp_del_cb, NULL, inst);
             e_gadcon_popup_show(inst->popup);
             e_object_data_set(E_OBJECT(inst->popup), inst);
             E_OBJECT_DEL_SET(inst->popup, _popup_del_cb);
          }
     }
}

static E_Gadcon_Client *
_gc_init(E_Gadcon *gc, const char *name, const char *id, const char *style)
{
   Instance *inst;
   E_Gadcon_Client *gcc;
   char buf[4096];

   _config_init();
   inst = _instance_create();

   snprintf(buf, sizeof(buf), "%s/dfu.edj", e_module_dir_get(_module));

   inst->o_icon = edje_object_add(gc->evas);
   if (!e_theme_edje_object_set(inst->o_icon,
				"base/theme/modules/dfu",
                                "modules/dfu/main"))
      edje_object_file_set(inst->o_icon, buf, "modules/dfu/main");
   evas_object_show(inst->o_icon);

   gcc = e_gadcon_client_new(gc, name, id, style, inst->o_icon);
   gcc->data = inst;
   inst->gcc = gcc;

   evas_object_event_callback_add(inst->o_icon, EVAS_CALLBACK_MOUSE_DOWN,
				  _button_cb_mouse_down, inst);

   eeze_udev_watch_add(EEZE_UDEV_TYPE_NONE, EEZE_UDEV_EVENT_ADD, _udev_added_cb, NULL);

   return gcc;
}

static void
_gc_shutdown(E_Gadcon_Client *gcc)
{
   _instance_delete(gcc->data);
   _config_shutdown();
}

static void
_gc_orient(E_Gadcon_Client *gcc, E_Gadcon_Orient orient EINA_UNUSED)
{
   e_gadcon_client_aspect_set(gcc, 32, 16);
   e_gadcon_client_min_size_set(gcc, 32, 16);
}

static const char *
_gc_label(const E_Gadcon_Client_Class *client_class EINA_UNUSED)
{
   return "DFU";
}

static Evas_Object *
_gc_icon(const E_Gadcon_Client_Class *client_class EINA_UNUSED, Evas *evas)
{
   Evas_Object *o;
   char buf[4096];

   if (!_module) return NULL;

   snprintf(buf, sizeof(buf), "%s/e-module-dfu.edj", e_module_dir_get(_module));

   o = edje_object_add(evas);
   edje_object_file_set(o, buf, "icon");
   return o;
}

static const char *
_gc_id_new(const E_Gadcon_Client_Class *client_class)
{
   char buf[32];
   static int id = 0;
   sprintf(buf, "%s.%d", client_class->name, ++id);
   return eina_stringshare_add(buf);
}

EAPI E_Module_Api e_modapi =
{
   E_MODULE_API_VERSION, "DFU"
};

static const E_Gadcon_Client_Class _gc_class =
{
   GADCON_CLIENT_CLASS_VERSION, "dfu",
   {
      _gc_init, _gc_shutdown, _gc_orient, _gc_label, _gc_icon, _gc_id_new, NULL, NULL
   },
   E_GADCON_CLIENT_STYLE_PLAIN
};

EAPI void *
e_modapi_init(E_Module *m)
{
   ecore_init();
   ecore_con_init();
   ecore_con_url_init();
   efreet_init();
   eeze_init();

   _module = m;
   e_gadcon_provider_register(&_gc_class);

   return m;
}

EAPI int
e_modapi_shutdown(E_Module *m EINA_UNUSED)
{
   e_gadcon_provider_unregister(&_gc_class);

   _module = NULL;
   eeze_shutdown();
   efreet_shutdown();
   ecore_con_url_shutdown();
   ecore_con_shutdown();
   ecore_shutdown();
   return 1;
}

EAPI int
e_modapi_save(E_Module *m EINA_UNUSED)
{
   return 1;
}
