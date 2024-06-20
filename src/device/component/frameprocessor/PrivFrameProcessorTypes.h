#pragma once
#include "openobsdk/h/Device.h"
#include "openobsdk/h/Error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ob_frame_processor_context_t ob_frame_processor_context;
typedef struct ob_frame_processor_t ob_frame_processor;

typedef ob_frame_processor_context* (*pfunc_ob_create_frame_processor_context)(ob_device *device,ob_error **error);

typedef ob_frame_processor* (*pfunc_ob_create_frame_processor)(ob_frame_processor_context *context,ob_sensor_type type,ob_error **error);

typedef const char * (*pfunc_ob_frame_processor_get_config_schema)(ob_frame_processor* processor,ob_error **error);

typedef void (*pfunc_ob_frame_processor_update_config)(ob_frame_processor* processor, size_t argc, const char **argv, ob_error **error);

typedef ob_frame* (*pfunc_ob_frame_processor_process_frame)(ob_frame_processor *processor,ob_frame *frame,ob_error **error);

typedef void (*pfunc_ob_destroy_frame_processor)(ob_frame_processor *processor,ob_error **error);

typedef void (*pfunc_ob_destroy_frame_processor_context)(ob_frame_processor_context *context,ob_error **error);


#ifdef __cplusplus
}
#endif