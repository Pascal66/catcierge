
#include "catcierge_config.h"
#include "catcierge_args.h"
#include "catcierge_log.h"
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <opencv2/imgproc/imgproc_c.h>
#include <opencv2/highgui/highgui_c.h>

#include "catcierge_util.h"
#include "catcierge_template_matcher.h"
#include "catcierge_haar_matcher.h"
#include "catcierge_timer.h"

#ifdef RPI
#include "RaspiCamCV.h"
#include "catcierge_gpio.h"
#endif

#ifdef CATCIERGE_HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef CATCIERGE_HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef CATCIERGE_HAVE_GRP_H
#include <grp.h>
#endif
#ifdef CATCIERGE_HAVE_PWD_H
#include <pwd.h>
#endif

#include "catcierge_fsm.h"
#include "catcierge_output.h"

void catcierge_run_state(catcierge_grb_t *grb)
{
	assert(grb);
	assert(grb->state);
	grb->state(grb);
}

const char *catcierge_get_state_string(catcierge_state_func_t state)
{
	if (state == catcierge_state_waiting) return "Waiting";
	if (state == catcierge_state_matching) return "Matching";
	if (state == catcierge_state_keepopen) return "Keep open";
	if (state == catcierge_state_lockout) return "Lockout";

	return "Initial";
}

void catcierge_print_state(catcierge_state_func_t state)
{
	log_printf(stdout, COLOR_NORMAL, "[");
	log_printf(stdout, COLOR_YELLOW, "%s",
		catcierge_get_state_string(state));
	log_printf(stdout, COLOR_NORMAL, "]");
}

void catcierge_set_state(catcierge_grb_t *grb, catcierge_state_func_t new_state)
{
	assert(grb);

	// Prints timestamp.
	log_printc(stdout, COLOR_NORMAL, "");

	catcierge_print_state(grb->state);
	log_printf(stdout, COLOR_MAGNETA, " -> ");
	catcierge_print_state(new_state);
	log_printf(stdout, COLOR_NORMAL, "\n");

	grb->prev_state = grb->state;
	grb->state = new_state;
}

#ifdef WITH_RFID
static int match_allowed_rfid(catcierge_grb_t *grb, const char *rfid_tag)
{
	int i;
	catcierge_args_t *args;
	assert(grb);
	args = &grb->args;

	for (i = 0; i < args->rfid_allowed_count; i++)
	{
		if (!strcmp(rfid_tag, args->rfid_allowed[i]))
		{
			return 1;
		}
	}

	return 0;
}

static void rfid_set_direction(catcierge_grb_t *grb, rfid_match_t *current, rfid_match_t *other, 
						match_direction_t dir, const char *dir_str, catcierge_rfid_t *rfid, 
						int complete, const char *data, size_t data_len)
{
	catcierge_args_t *args;
	assert(grb);
	args = &grb->args;

	CATLOG("%s RFID: %s%s\n", rfid->name, data, !complete ? " (incomplete)": "");

	CATLOG("   Old data %s (%d bytes) New data %s (%d bytes)\n",
		other->complete ? "COMPLETE" : "INCOMPLETE",
		(int)other->data_len,
		complete ? "COMPLETE" : "INCOMPLETE",
		(int)data_len);

	// Update the match if we get a complete tag.
	if (complete && (data_len > other->data_len))
	{
		strncpy(current->data, data, sizeof(current->data));
		current->data_len = data_len;
		current->complete = complete;
	}

	// If we have already triggered this reader
	// then don't set the direction again.
	// (Since this could screw things up).
	if (current->triggered)
	{
		CATLOG("Already triggered!\n");
		return;
	}

	// The other reader triggered first so we know the direction.
	// TODO: It could be wise to time this out after a while...
	if (other->triggered)
	{
		grb->rfid_direction = dir;
		CATLOG("%s RFID: Direction %s\n", rfid->name, dir_str);
	}

	current->triggered = 1;
	current->complete = complete;
	strncpy(current->data, data, sizeof(current->data));
	current->data_len = data_len;
	current->is_allowed = match_allowed_rfid(grb, current->data);

	//log_print_csv(log_file, "rfid, %s, %s\n", 
	//		current->data, (current->is_allowed > 0)? "allowed" : "rejected");

	if (args->new_execute)
	{
		// TODO: Do we have all RFID vars for this?
		catcierge_output_execute(grb, "rfid_detect", args->rfid_detect_cmd);
	}
	else
	{
		catcierge_execute(args->rfid_detect_cmd, 
			"%s %s %d %d %s %d %s",
			rfid->name, 				// %0 = RFID reader name.
			rfid->serial_path,			// %1 = RFID path.
			current->is_allowed, 		// %2 = Is allowed.
			!current->complete, 		// %3 = Is data incomplete.
			current->data,				// %4 = Tag data.
			other->triggered,			// %5 = Other reader triggered.
			catcierge_get_direction_str(grb->rfid_direction)); // %6 = Direction.
	}
}

static void rfid_inner_read_cb(catcierge_rfid_t *rfid, int complete, const char *data, size_t data_len, void *user)
{
	catcierge_grb_t *grb = user;

	// The inner RFID reader has detected a tag, we now pass that
	// match on to the code that decides which direction the cat
	// is going.
	rfid_set_direction(grb, &grb->rfid_in_match, &grb->rfid_out_match,
			MATCH_DIR_IN, "IN", rfid, complete, data, data_len);
}

static void rfid_outer_read_cb(catcierge_rfid_t *rfid, int complete, const char *data, size_t data_len, void *user)
{
	catcierge_grb_t *grb = user;
	rfid_set_direction(grb, &grb->rfid_out_match, &grb->rfid_in_match,
			MATCH_DIR_OUT, "OUT", rfid, complete, data, data_len);
}

void catcierge_init_rfid_readers(catcierge_grb_t *grb)
{
	catcierge_args_t *args;
	assert(grb);

	args = &grb->args;

	catcierge_rfid_ctx_init(&grb->rfid_ctx);

	if (args->rfid_inner_path)
	{
		memset(&grb->rfid_in_match, 0, sizeof(grb->rfid_in_match));
		catcierge_rfid_init("Inner", &grb->rfid_in, args->rfid_inner_path, rfid_inner_read_cb, grb);
		catcierge_rfid_ctx_set_inner(&grb->rfid_ctx, &grb->rfid_in);
		catcierge_rfid_open(&grb->rfid_in);
	}

	if (args->rfid_outer_path)
	{
		memset(&grb->rfid_out_match, 0, sizeof(grb->rfid_out_match));
		catcierge_rfid_init("Outer", &grb->rfid_out, args->rfid_outer_path, rfid_outer_read_cb, grb);
		catcierge_rfid_ctx_set_outer(&grb->rfid_ctx, &grb->rfid_out);
		catcierge_rfid_open(&grb->rfid_out);
	}
	
	CATLOG("Initialized RFID readers\n");
}
#endif // WITH_RFID

void catcierge_do_lockout(catcierge_grb_t *grb)
{
	catcierge_args_t *args;
	assert(grb);
	args = &grb->args;

	if (args->lockout_dummy)
	{
		CATLOGFPS("!LOCKOUT DUMMY!\n");
		return;
	}

	if (args->do_lockout_cmd)
	{
		if (args->new_execute)
		{
			catcierge_output_execute(grb, "do_lockout", args->do_lockout_cmd);
		}
		else
		{
			catcierge_execute(args->do_lockout_cmd, "");
		}
	}
	else
	{
		#ifdef RPI
		gpio_write(DOOR_PIN, 1);
		gpio_write(BACKLIGHT_PIN, 1);
		#endif // RPI
	}
}

void catcierge_do_unlock(catcierge_grb_t *grb)
{
	catcierge_args_t *args;
	assert(grb);
	args = &grb->args;

	if (args->do_unlock_cmd)
	{
		if (args->new_execute)
		{
			catcierge_output_execute(grb, "do_unlock", args->do_unlock_cmd);
		}
		else
		{
			catcierge_execute(args->do_unlock_cmd, "");
		}
	}
	else
	{
		#ifdef RPI
		gpio_write(DOOR_PIN, 0);
		gpio_write(BACKLIGHT_PIN, 1);
		#endif // RPI
	}
}

static void catcierge_cleanup_match_steps(catcierge_grb_t *grb, match_result_t *result)
{
	int j;
	match_step_t *step = NULL;
	assert(grb);
	assert(result);

	for (j = 0; j < MAX_MATCH_RECTS; j++)
	{
		step = &result->steps[j];

		if (step->img)
		{
			cvReleaseImage(&step->img);
			step->img = NULL;
		}

		step->description = NULL;
		step->name = NULL;
		step->path[0] = '\0';
	}

	result->step_img_count = 0;
}

static void catcierge_cleanup_imgs(catcierge_grb_t *grb)
{
	int i;
	assert(grb);

	for (i = 0; i < MATCH_MAX_COUNT; i++)
	{
		if (grb->match_group.matches[i].img)
		{
			cvReleaseImage(&grb->match_group.matches[i].img);
			grb->match_group.matches[i].img = NULL;
		}

		catcierge_cleanup_match_steps(grb, &grb->match_group.matches[i].result);
	}
}

void catcierge_setup_camera(catcierge_grb_t *grb)
{
	assert(grb);

	#ifdef RPI
	grb->capture = raspiCamCvCreateCameraCaptureEx(0, &grb->args.rpi_settings);
	#else
	grb->capture = cvCreateCameraCapture(0);
	cvSetCaptureProperty(grb->capture, CV_CAP_PROP_FRAME_WIDTH, 320);
	cvSetCaptureProperty(grb->capture, CV_CAP_PROP_FRAME_HEIGHT, 240);
	#endif

	if (grb->args.show)
	{
		cvNamedWindow("catcierge", 1);
	}
}

void catcierge_destroy_camera(catcierge_grb_t *grb)
{
	if (grb->args.show)
	{
		cvDestroyWindow("catcierge");
	}

	#ifdef RPI
	raspiCamCvReleaseCapture(&grb->capture);
	#else
	cvReleaseCapture(&grb->capture);
	#endif
}

int catcierge_drop_root_privileges(const char *user)
{
	#ifdef CATCIERGE_ENABLE_DROP_ROOT_PRIVILEGES
	struct passwd *pw = getpwnam(user);

	if (pw->pw_uid != 0)
	{
		CATLOG("Not running as root (no privileges to drop).\n");
		return 0;
	}

	if (initgroups(pw->pw_name, pw->pw_gid)
	 || setgid(pw->pw_gid)
	 || setuid(pw->pw_uid))
	{
		CATERR("Failed to drop root privileges '%.32s' uid=%lu gid=%lu: %s\n",
			user, (unsigned long)pw->pw_uid, (unsigned long)pw->pw_gid,
			strerror(errno));
		return -1;
	}
	#endif // CATCIERGE_ENABLE_DROP_ROOT_PRIVILEGES
	return 0;
}

#ifdef RPI
int catcierge_setup_gpio(catcierge_grb_t *grb)
{
	catcierge_args_t *args = &grb->args;
	int ret = 0;

	// Set export for pins.
	if (gpio_export(DOOR_PIN) || gpio_set_direction(DOOR_PIN, OUT))
	{
		CATERRFPS("Failed to export and set direction for door pin\n");
		ret = -1;
		goto fail;
	}

	if (gpio_export(BACKLIGHT_PIN) || gpio_set_direction(BACKLIGHT_PIN, OUT))
	{
		CATERRFPS("Failed to export and set direction for backlight pin\n");
		ret = -1;
		goto fail;
	}

	// Start with the door open and light on.
	gpio_write(DOOR_PIN, 0);
	gpio_write(BACKLIGHT_PIN, 1);

fail:
	if (ret)
	{
		// Check if we're root.
		if (getuid() != 0)
		{
			CATERR("###############################################\n");
			CATERR("########## You have to run as root! ###########\n");
			CATERR("###############################################\n");
		}
	}
	else if (args->chuid && (getuid() == 0))
	{
		if (!catcierge_drop_root_privileges(args->chuid))
		{
			CATLOG("###############################################\n");
			CATLOG("########## Root privileges dropped ############\n");
			CATLOG("###############################################\n");
		}
	}

	return ret;
}
#endif // RPI

IplImage *catcierge_get_frame(catcierge_grb_t *grb)
{
	assert(grb);

	#ifdef RPI
	return raspiCamCvQueryFrame(grb->capture);
	#else
	return cvQueryFrame(grb->capture);
	#endif	
}

static int catcierge_calculate_match_id(IplImage *img, match_state_t *m)
{
	assert(img);
	assert(m);

	// Get a unique match id by calculating SHA1 hash of the image data
	// as well as timestamp.
	SHA1Reset(&m->sha);
	SHA1Input(&m->sha, (const unsigned char *)img->imageData, img->imageSize);
	SHA1Input(&m->sha, (const unsigned char *)m->time_str, strlen(m->time_str));

	if (!SHA1Result(&m->sha))
	{
		return -1;
	}

	return 0;
}

static void catcierge_process_match_result(catcierge_grb_t *grb,
				IplImage *img, match_state_t *m)
{
	size_t j;
	catcierge_args_t *args = NULL;
	match_result_t *res = NULL;
	assert(grb);
	assert(img);
	assert(m);
	args = &grb->args;

	res = &m->result;

	// Get time of match and format.
	m->img = NULL;
	m->time = time(NULL);
	gettimeofday(&m->tv, NULL);
	get_time_str_fmt(m->time, &m->tv, m->time_str,
		sizeof(m->time_str), "%Y-%m-%d_%H_%M_%S.%f");

	// Calculate match id from time + image data.
	if (catcierge_calculate_match_id(img, m))
	{
		CATERR("Failed to calculate match id!\n");
	}

	log_printc(stdout, (res->success ? COLOR_GREEN : COLOR_RED),
		"%sMatch %s - %s (%x%x%x%x%x)\n",
		res->success ? "" : "No ",
		catcierge_get_direction_str(res->direction),
		res->description,
		m->sha.Message_Digest[0],
		m->sha.Message_Digest[1],
		m->sha.Message_Digest[2],
		m->sha.Message_Digest[3],
		m->sha.Message_Digest[4]);

	m->path[0] = '\0';

	// Save match image.
	// (We don't write to disk yet, that will slow down the matching).
	if (args->saveimg)
	{
		char base_path[1024];

		snprintf(base_path,
			sizeof(base_path) - 1,
			"%s%smatch_%s_%s__%d",
			args->output_path ? args->output_path : ".",
			catcierge_path_sep(),
			res->success ? "" : "fail",
			m->time_str,
			(int)grb->match_group.match_count);

		snprintf(m->path, sizeof(m->path) - 1, "%s.png", base_path);

		m->img = cvCloneImage(img);

		if (args->save_steps)
		{
			match_step_t *step;

			for (j = 0; j < m->result.step_img_count; j++)
			{
				step = &m->result.steps[j];
				snprintf(step->path, sizeof(step->path) - 1,
					"%s_%02d_%s.png",
					base_path, (int)j, step->name);
			}
		}
	}

	// Log match to file.
	log_print_csv(grb->log_file, "match, %s, %f, %f, %s, %s\n",
		 res->success ? "success" : "failure",
		 res->result, args->templ.match_threshold,
		 args->saveimg ? m->path : "-",
		 catcierge_get_direction_str(res->direction));
}

static void catcierge_save_images(catcierge_grb_t *grb, match_direction_t direction)
{
	match_state_t *m;
	match_result_t *res;
	int i;
	size_t j;
	catcierge_args_t *args;
	match_step_t *step = NULL;
	assert(grb);
	args = &grb->args;

	for (i = 0; i < MATCH_MAX_COUNT; i++)
	{
		m = &grb->match_group.matches[i];
		res = &m->result;
		CATLOG("Saving image %s\n", m->path);
		cvSaveImage(m->path, m->img, 0);

		if (args->save_steps)
		{
			for (j = 0; j < m->result.step_img_count; j++)
			{
				step = &m->result.steps[j];
				CATLOG("  %02d %-34s  %s\n", (int)j, step->description, step->path);

				if (step->img)
					cvSaveImage(step->path, step->img, NULL);
			}
		}

		if (args->new_execute)
		{
			catcierge_output_execute(grb, "save_img", args->save_img_cmd);
		}
		else
		{
			catcierge_execute(args->save_img_cmd, "%f %d %s %d",
				res->result,	// %0 = Match result.
				res->success, 	// %1 = Match success.
				m->path,		// %2 = Image path (of now saved image).
				res->direction);// %3 = Match direction.
		}

		cvReleaseImage(&m->img);
		m->img = NULL;
	}

	if (args->new_execute)
	{
		catcierge_output_execute(grb, "save_imgs", args->save_imgs_cmd);
	}
	else
	{
		catcierge_execute(args->save_imgs_cmd,
			"%d %s %s %s %s %f %f %f %f %d %d %d %d %d",
			grb->match_group.success, 						// %0 = Match success.
			grb->match_group.matches[0].path,				// %1 = Image 1 path (of now saved image).
			grb->match_group.matches[1].path,				// %2 = Image 2 path (of now saved image).
			grb->match_group.matches[2].path,				// %3 = Image 3 path (of now saved image).
			grb->match_group.matches[3].path,				// %4 = Image 4 path (of now saved image).
			grb->match_group.matches[0].result.result,		// %5 = Image 1 result.
			grb->match_group.matches[1].result.result,		// %6 = Image 2 result.
			grb->match_group.matches[2].result.result,		// %7 = Image 3 result.
			grb->match_group.matches[3].result.result,		// %8 = Image 4 result.
			grb->match_group.matches[0].result.direction,	// %9 =  Image 1 direction.
			grb->match_group.matches[1].result.direction,	// %10 = Image 2 direction.
			grb->match_group.matches[2].result.direction,	// %11 = Image 3 direction.
			grb->match_group.matches[3].result.direction,	// %12 = Image 4 direction.
			direction); 									// %13 = Total direction.
	}
}

static void catcierge_check_max_consecutive_lockouts(catcierge_grb_t *grb)
{
	catcierge_args_t *args;
	assert(grb);
	args = &grb->args;

	// Check how long ago we perform the last lockout.
	// If there are too many lockouts in a row, there
	// might be an error. Such as the backlight failing.
	if (args->max_consecutive_lockout_count)
	{
		double lockout_timer_val = catcierge_timer_get(&grb->lockout_timer);

		if ((lockout_timer_val
			<= (args->lockout_time + args->consecutive_lockout_delay)))
		{
			grb->consecutive_lockout_count++;
			CATLOG("Consecutive lockout! %d out of %d before quiting. "
				   "(%0.2f sec <= %0.2f sec)\n",
					grb->consecutive_lockout_count, 
					args->max_consecutive_lockout_count,
					lockout_timer_val,
					(args->lockout_time + args->consecutive_lockout_delay));
		}
		else
		{
			grb->consecutive_lockout_count = 0;

			CATLOG("Consecutive lockout count reset. "
					"%0.2f seconds between lockouts "
					"(consecutive lockout delay = %0.2f seconds)\n",
					catcierge_timer_get(&grb->lockout_timer),
					args->consecutive_lockout_delay);
		}

		// Exit the program, we assume we have an error.
		if (grb->consecutive_lockout_count >= args->max_consecutive_lockout_count)
		{
			CATLOG("Too many lockouts in a row (%d)! Assuming something is wrong... Aborting program!\n",
						grb->consecutive_lockout_count);
			catcierge_do_unlock(grb);
			// TODO: Add a special event the user can trigger an external program with here...
			grb->running = 0;
			//exit(1);
		}
	}
}

#ifdef WITH_RFID
static void catcierge_should_we_rfid_lockout(catcierge_grb_t *grb)
{
	catcierge_args_t *args;
	assert(grb);
	args = &grb->args;

	if (!args->lock_on_invalid_rfid)
		return;

	if (!grb->checked_rfid_lock 
		&& (args->rfid_inner_path || args->rfid_outer_path))
	{
		// Have we waited long enough since the camera match was
		// complete (The cat must have moved far enough for both
		// readers to have a chance to detect it).
		if (catcierge_timer_get(&grb->rematch_timer) >= args->rfid_lock_time)
		{
			int do_rfid_lockout = 0;

			if (!grb->rfid_out_match.triggered && !grb->rfid_in_match.triggered)
			{
				CATERR("Unknown RFID direction!\n");
				grb->rfid_direction = MATCH_DIR_UNKNOWN;
			}

			if (args->rfid_inner_path && args->rfid_outer_path)
			{
				// Only require one of the readers to have a correct read.
				do_rfid_lockout = !(grb->rfid_in_match.is_allowed 
								|| grb->rfid_out_match.is_allowed);
			}
			else if (args->rfid_inner_path)
			{
				do_rfid_lockout = !grb->rfid_in_match.is_allowed;
			}
			else if (args->rfid_outer_path)
			{
				do_rfid_lockout = !grb->rfid_out_match.is_allowed;
			}

			if (do_rfid_lockout)
			{
				if (grb->rfid_direction == MATCH_DIR_OUT)
				{
					CATLOG("RFID lockout: Skipping since cat is going out\n");
				}
				else
				{
					CATLOG("RFID lockout!\n");
					log_print_csv(grb->log_file, "rfid_check, lockout\n");
					catcierge_state_transition_lockout(grb);
				}
			}
			else
			{
				CATLOG("RFID OK!\n");
				log_print_csv(grb->log_file, "rfid_check, ok\n");
			}

			if (args->rfid_inner_path) CATLOG("  %s RFID: %s\n", grb->rfid_in.name, grb->rfid_in_match.triggered ? grb->rfid_in_match.data : "No tag data");
			if (args->rfid_outer_path) CATLOG("  %s RFID: %s\n", grb->rfid_out.name, grb->rfid_out_match.triggered ? grb->rfid_out_match.data : "No tag data");

			if (args->new_execute)
			{
				// TODO: Do we have all RFID vars for this?
				catcierge_output_execute(grb, "rfid_match", args->rfid_match_cmd);
			}
			else
			{
				// %0 = Match success.
				// %1 = RFID inner in use.
				// %2 = RFID outer in use.
				// %3 = RFID inner success.
				// %4 = RFID outer success.
				// %5 = RFID inner data.
				// %6 = RFID outer data.
				catcierge_execute(args->rfid_match_cmd, 
					"%d %d %d %d %s %s", 
					!do_rfid_lockout,
					(args->rfid_inner_path != NULL),
					(args->rfid_outer_path != NULL),
					grb->rfid_in_match.is_allowed,
					grb->rfid_out_match.is_allowed,
					grb->rfid_in_match.data,
					grb->rfid_out_match.data);
			}

			grb->checked_rfid_lock = 1;
		}
	}
}
#endif // WITH_RFID

static void catcierge_show_image(catcierge_grb_t *grb)
{
	catcierge_args_t *args;
	match_state_t *m;
	match_result_t *res;
	IplImage *img;
	IplImage *tmp_img = NULL;
	assert(grb);
	args = &grb->args;

	if (!grb->img)
		return;

	// Show the video feed.
	if (args->show)
	{
		img = grb->img;

		// Only try to show the match rectangles when we're in match mode.
		if ((grb->match_group.match_count > 0)
			&& (grb->match_group.match_count <= MATCH_MAX_COUNT))
		{
			size_t i;
			CvScalar match_color;

			// We don't want to mess with the original image when
			// drawing the match rects since that might interfer with the match.
			tmp_img = cvCloneImage(grb->img);

			m = &grb->match_group.matches[grb->match_group.match_count - 1];
			res = &m->result;

			#ifdef RPI
			match_color = CV_RGB(255, 255, 255); // Grayscale so don't bother with color.
			#else
			match_color = (res->success) ? CV_RGB(0, 255, 0) : CV_RGB(255, 0, 0);
			#endif

			// Always highlight when showing in GUI.
			for (i = 0; i < res->rect_count; i++)
			{
				cvRectangleR(tmp_img, res->match_rects[i], match_color, 2, 8, 0);
			}

			img = tmp_img;
		}

		cvShowImage("catcierge", img);
		cvWaitKey(10);

		if (tmp_img)
		{
			cvReleaseImage(&tmp_img);
		}
	}
}

double catcierge_do_match(catcierge_grb_t *grb, match_result_t *result)
{
	double match_res = 0.0;
	catcierge_args_t *args;
	assert(grb);
	args = &grb->args;

	catcierge_cleanup_match_steps(grb, result);

	if (grb->args.matcher_type == MATCHER_TEMPLATE)
	{
		if ((match_res = catcierge_template_matcher_match(&grb->matcher,
							grb->img, result, args->save_steps)) < 0.0)
		{
			CATERR("Template matcher: Error when matching frame!\n");
		}
	}
	else
	{
		// Haar matcher.
		if ((match_res = catcierge_haar_matcher_match(&grb->haar,
							grb->img, result, args->save_steps)) < 0.0)
		{
			CATERR("Haar matcher: Error when matching frame!\n");
		}
	}

	return match_res;
}

// =============================================================================
// States
// =============================================================================
int catcierge_state_waiting(catcierge_grb_t *grb);

int catcierge_state_keepopen(catcierge_grb_t *grb)
{
	assert(grb);

	catcierge_show_image(grb);

	// Wait until the frame is clear before we start the timer.
	// When this timer ends, we will go back to the WAITING state.
	if (!catcierge_timer_isactive(&grb->rematch_timer))
	{
		// We have successfully matched a valid cat :D
		int frame_obstructed;

		if ((frame_obstructed = catcierge_is_frame_obstructed(grb->img, 0)) < 0)
		{
			CATERR("Failed to run check for obstructed frame\n");
			return -1;
		}

		if (frame_obstructed)
		{
			return 0;
		}

		CATLOG("Frame is clear, start successful match timer...\n");
		catcierge_timer_set(&grb->rematch_timer, grb->args.match_time);
		catcierge_timer_start(&grb->rematch_timer);
	}

	if (catcierge_timer_has_timed_out(&grb->rematch_timer))
	{
		CATLOG("Go back to waiting...\n");
		catcierge_set_state(grb, catcierge_state_waiting);
		return 0;
	}

	#ifdef WITH_RFID
	// The check to block on RFID is performed at a delay after the
	// image matching has been performed, to give the cat time to
	// pass both RFID readers.
	catcierge_should_we_rfid_lockout(grb);
	#endif

	return 0;
}

int catcierge_state_lockout(catcierge_grb_t *grb)
{
	int frame_obstructed;
	catcierge_args_t *args;
	assert(grb);
	args = &grb->args;

	catcierge_show_image(grb);

	if (args->lockout_method == OBSTRUCT_OR_TIMER_1)
	{
		// Stop the lockout when frame is clear
		// OR if the lockout timer ends.

		if ((frame_obstructed = catcierge_is_frame_obstructed(grb->img, 0)) < 0)
		{
			CATERR("Failed to run check for obstructed frame\n");
			return -1;
		}

		if (!frame_obstructed || catcierge_timer_has_timed_out(&grb->lockout_timer))
		{
			CATLOG("End of lockout!\n");
			catcierge_do_unlock(grb);
			catcierge_set_state(grb, catcierge_state_waiting);
			return 0;
		}
	}
	else if (args->lockout_method == OBSTRUCT_THEN_TIMER_2)
	{
		// Don't start the lockout timer until the frame becomes clear.

		if (!catcierge_timer_isactive(&grb->lockout_timer))
		{
			if ((frame_obstructed = catcierge_is_frame_obstructed(grb->img, 0)) < 0)
			{
				CATERR("Failed to run check for obstructed frame\n");
				return -1;
			}

			if (frame_obstructed)
			{
				return 0;
			}

			CATLOG("Frame is clear, start lockout timer...\n\n");
			catcierge_timer_set(&grb->lockout_timer, grb->args.lockout_time);
			catcierge_timer_start(&grb->lockout_timer);
		}
	}

	if (catcierge_timer_has_timed_out(&grb->lockout_timer))
	{
		CATLOG("End of lockout! (timed out after %f seconds)\n",
			catcierge_timer_get(&grb->lockout_timer));

		catcierge_do_unlock(grb);
		catcierge_set_state(grb, catcierge_state_waiting);
	}
	return 0;
}

void catcierge_state_transition_lockout(catcierge_grb_t *grb)
{
	catcierge_args_t *args;
	assert(grb);
	args = &grb->args;

	// Start the timer up front depending on lockout method.
	switch (args->lockout_method)
	{
		default: break;
		case OBSTRUCT_THEN_TIMER_2:
			catcierge_timer_reset(&grb->lockout_timer);
			break;
		case OBSTRUCT_OR_TIMER_1:
		case TIMER_ONLY_3:
			catcierge_timer_set(&grb->lockout_timer, args->lockout_time);
			catcierge_timer_start(&grb->lockout_timer);
			break;
	}

	catcierge_set_state(grb, catcierge_state_lockout);
	catcierge_do_lockout(grb);
}

static match_direction_t catcierge_guess_overall_direction(catcierge_grb_t *grb)
{
	int i;
	match_direction_t direction = MATCH_DIR_UNKNOWN;
	assert(grb);

	if (grb->args.matcher_type == MATCHER_TEMPLATE)
	{
		// Get any successful direction.
		// (It is very uncommon for 2 successful matches to give different
		// direction with the template matcher, so we can be pretty sure
		// this is correct).
		for (i = 0; i < MATCH_MAX_COUNT; i++)
		{
			if (grb->match_group.matches[i].result.success)
			{
				direction = grb->match_group.matches[i].result.direction;
			}
		}
	}
	else
	{
		// Count the actual direction counts for the haar matcher.
		int in_count = 0;
		int out_count = 0;
		int unknown_count = 0;

		for (i = 0; i < MATCH_MAX_COUNT; i++)
		{
			switch (grb->match_group.matches[i].result.direction)
			{
				case MATCH_DIR_IN: in_count++; break;
				case MATCH_DIR_OUT: out_count++; break;
				case MATCH_DIR_UNKNOWN: unknown_count++; break;
			}
		}

		if ((in_count > out_count) && (in_count > unknown_count))
		{
			direction = MATCH_DIR_IN;
		}
		else if (out_count > unknown_count)
		{
			direction = MATCH_DIR_OUT;
		}
		else
		{
			direction = MATCH_DIR_UNKNOWN;
		}
	}

	return direction;
}

int catcierge_decide_lock_status(catcierge_grb_t *grb)
{
	match_group_t *mg = &grb->match_group;
	assert(grb);

	// TODO: Move the lock deciding code in catcierge_state_matching here.

	return 0;
}

int catcierge_state_matching(catcierge_grb_t *grb)
{
	catcierge_args_t *args;
	match_state_t *match = NULL;
	match_result_t *result;
	assert(grb);
	args = &grb->args;

	// TODO: Start a matchgroup timer here if grb->match_count == 0

	match = &grb->match_group.matches[grb->match_group.match_count];
	result = &match->result;
	memset(result, 0, sizeof(match_result_t));

	// We have something to match against.
	if (catcierge_do_match(grb, result) < 0)
	{
		CATERRFPS("Error when matching frame!\n");
		return -1;
	}

	// TODO: Redo this function to add the match to the match group struct.
	catcierge_process_match_result(grb, grb->img, match);

	grb->match_group.match_count++;

	// Runs the --match_cmd program specified.
	if (args->new_execute)
	{
		catcierge_output_execute(grb, "match", args->match_cmd);
	}
	else
	{
		catcierge_execute(args->match_cmd, "%f %d %s %d",
				result->result, 					// %0 = Match result.
				result->success,					// %1 = 0/1 succes or failure.
				args->saveimg ? match->path : "",	// %2 = Image path if saveimg is turned on.
				result->direction);					// %3 = Direction, 0 = in, 1 = out.
	}

	catcierge_show_image(grb);

	if (grb->match_group.match_count < MATCH_MAX_COUNT)
	{
		// Continue until we have enough matches for a decision.
		return 0;
	}
	else
	{
		// We now have enough images to decide lock status.
		int i;

		// TODO: grb->match_success and this direction to a "match group struct" instead.
		// This means this can be refactored into a function as well.
		match_direction_t direction;
		grb->match_group.success = 0;
		grb->match_group.success_count = 0;

		for (i = 0; i < MATCH_MAX_COUNT; i++)
		{
			grb->match_group.success_count += !!grb->match_group.matches[i].result.success;
		}

		// Guess the direction.
		direction = catcierge_guess_overall_direction(grb);

		// When going out, if only 1 image is a succesful match
		// we still count it as overall succesful so we don't get
		// so many false negatives.
		if (direction == MATCH_DIR_OUT)
		{
			grb->match_group.success = 1;
		}
		else
		{
			// Otherwise if enough matches (default 2) are ok.
			grb->match_group.success = (grb->match_group.success_count >= args->ok_matches_needed);

			// TODO: Let the matcher veto if the match group was successful:
			// For instance call catcierge_haar_matcher_decide.
			// If for instance the haar matcher finds no cat face in any image
			// perform a lockout anyway.
			// Make this a command line option to enable.
		}

		if (grb->match_group.success)
		{
			CATLOG("Everything OK! (%d out of %d matches succeeded)"
					" Door kept open...\n", grb->match_group.success_count, MATCH_MAX_COUNT);

			if (grb->consecutive_lockout_count > 0)
			{
				grb->consecutive_lockout_count = 0;
				CATLOG("Consecutive lockout count reset\n");
			}

			// Make sure the door is open.
			catcierge_do_unlock(grb);

			#ifdef WITH_RFID
			// We only want to check for RFID lock once
			// during each match timeout period.
			grb->checked_rfid_lock = 0;
			#endif

			catcierge_timer_reset(&grb->rematch_timer);
			catcierge_set_state(grb, catcierge_state_keepopen);
		}
		else
		{
			CATLOG("Lockout! %d out of %d matches failed.\n",
					(MATCH_MAX_COUNT - grb->match_group.success_count), MATCH_MAX_COUNT);

			catcierge_check_max_consecutive_lockouts(grb);
			catcierge_state_transition_lockout(grb);
		}

		// TODO: End matchgroup timer here and create a unique ID for it.

		if (args->new_execute)
		{
			catcierge_output_execute(grb, "match_done", args->match_done_cmd);
		}
		else
		{
			catcierge_execute(args->match_done_cmd, "%d %d %d", 
				grb->match_group.success, 		// %0 = Match success.
				grb->match_group.success_count,	// %1 = Successful match count.
				MATCH_MAX_COUNT);				// %2 = Max matches.
		}

		// Now we can save the images that we cached earlier 
		// without slowing down the matching FPS.
		if (args->saveimg)
		{
			catcierge_save_images(grb, direction);
		}
	}

	return 0;
}

int catcierge_state_waiting(catcierge_grb_t *grb)
{
	int frame_obstructed;
	assert(grb);

	catcierge_show_image(grb);

	// Wait until the middle of the frame is black
	// before we try to match anything.
	if ((frame_obstructed = catcierge_is_frame_obstructed(grb->img, 0)) < 0)
	{
		CATERRFPS("Failed to detect check for obstructed frame\n");
		return -1;
	}

	if (frame_obstructed)
	{
		CATLOG("Something in frame! Start matching...\n");
		grb->match_group.match_count = 0;
		catcierge_set_state(grb, catcierge_state_matching);
	}

	return 0;
}

void catcierge_print_spinner(catcierge_grb_t *grb)
{
	catcierge_args_t *args;
	assert(grb);
	args = &grb->args;

	if (catcierge_timer_has_timed_out(&grb->frame_timer))
	{
		char spinner[] = "\\|/-\\|/-";
		static int spinidx = 0;
		catcierge_timer_reset(&grb->frame_timer);

		if (args->noanim)
		{
			return;
		}

		// This prints the log timestamp.
		log_printc(stdout, COLOR_NORMAL, "");
		catcierge_print_state(grb->state);
		log_printf(stdout, COLOR_NORMAL, "  ");

		if (grb->state == catcierge_state_lockout)
		{
			log_printf(stdout, COLOR_RED, "Lockout for %d more seconds.\n",
				(int)(args->lockout_time - catcierge_timer_get(&grb->lockout_timer)));
		}
		else if (grb->state == catcierge_state_keepopen)
		{
			if (catcierge_timer_isactive(&grb->rematch_timer))
			{
				log_printf(stdout, COLOR_RED, "Waiting to match again for %d more seconds.\n",
					(int)(args->match_time - catcierge_timer_get(&grb->rematch_timer)));
			}
			else
			{
				log_printf(stdout, COLOR_NORMAL, "Frame is obstructed. Waiting for it to clear...\n");
			}
		}
		else
		{
			log_printf(stdout, COLOR_CYAN, "%c\n", spinner[spinidx++ % (sizeof(spinner) - 1)]);
		}

		// Moves the cursor back so that we print the spinner in place.
		catcierge_reset_cursor_position();
	}
}

int catcierge_grabber_init(catcierge_grb_t *grb)
{
	assert(grb);

	memset(grb, 0, sizeof(catcierge_grb_t));
	
	if (catcierge_args_init(&grb->args))
	{
		return -1;
	}

	return 0;
}

void catcierge_grabber_destroy(catcierge_grb_t *grb)
{
	catcierge_args_destroy(&grb->args);
	catcierge_cleanup_imgs(grb);
}
