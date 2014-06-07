
#include <assert.h>
#include "catcierge_haar_matcher.h"
#include "catcierge_haar_wrapper.h"
#include "catcierge_types.h"

int catcierge_haar_matcher_init(catcierge_haar_matcher_t *ctx, catcierge_haar_matcher_args_t *args)
{
	assert(args);
	assert(ctx);

	if (!args->cascade)
	{
		return -1;
	}

	if (!(ctx->cascade = cv2CascadeClassifier_create()))
	{
		return -1;
	}

	if (cv2CascadeClassifier_load(ctx->cascade, args->cascade))
	{
		return -1;
	}

	ctx->in_direction = args->in_direction;

	return 0;
}

void catcierge_haar_matcher_destroy(catcierge_haar_matcher_t *ctx)
{
	assert(ctx);
	if (ctx->cascade)
	{
		cv2CascadeClassifier_destroy(ctx->cascade);
	}
}

match_direction_t catcierge_haar_guess_direction(catcierge_haar_matcher_t *ctx, IplImage *img)
{
	assert(ctx);
	int left_sum;
	int right_sum;
	match_direction_t dir = MATCH_DIR_UNKNOWN;
	CvRect roi = cvGetImageROI(img);

	// Left.
	cvSetImageROI(img, cvRect(0, 0, 1, roi.height));
	left_sum = (int)cvSum(img).val[0];

	// Right.
	cvSetImageROI(img, cvRect(roi.width - 1, 0, 1, roi.height));
	right_sum = (int)cvSum(img).val[0];

	if (abs(left_sum - right_sum) > 25)
	{
		if (ctx->debug) printf("Left: %d, Right: %d\n", left_sum, right_sum);

		if (right_sum > left_sum)
		{
			// Going right.
			dir = (ctx->in_direction == DIR_RIGHT) ? MATCH_DIR_IN : MATCH_DIR_OUT;
		}
		else
		{
			// Going left.
			dir = (ctx->in_direction == DIR_LEFT) ? MATCH_DIR_IN : MATCH_DIR_OUT;
		}
	}

	cvSetImageROI(img, roi);

	return dir;
}

double catcierge_haar_matcher_match(catcierge_haar_matcher_t *ctx, IplImage *img,
		CvRect *match_rects, size_t *rect_count)
{
	assert(ctx);
	double ret = 0.0;
	IplImage *img_eq = NULL;
	IplImage *img_gray = NULL;
	IplImage *tmp = NULL;
	match_direction_t dir = MATCH_DIR_UNKNOWN;
	CvSize max_size;
	CvSize min_size;
	min_size.width = 80;
	min_size.height = 80;
	max_size.width = 0;
	max_size.height = 0;

	// Make gray scale if needed.
	if (img->nChannels != 1)
	{
		tmp = cvCreateImage(cvGetSize(img), 8, 1);
		cvCvtColor(img, tmp, CV_BGR2GRAY);
		img_gray = tmp;
	}
	else
	{
		img_gray = img;
	}

	// Equalize histogram.
	img_eq = cvCreateImage(cvGetSize(img), 8, 1);
	cvEqualizeHist(img_gray, img_eq);

	if (cv2CascadeClassifier_detectMultiScale(ctx->cascade,
			img_eq, match_rects, rect_count,
			1.1, 3, CV_HAAR_SCALE_IMAGE, &min_size, &max_size))
	{
		ret = -1.0;
		goto fail;
	}

	printf("Rect count: %zu\n", *rect_count);
	ret = (*rect_count > 0) ? 1.0 : 0.0;

	// TODO: Set image roi to match rect.
	// TODO: Threshold the image.

	// TODO: Either find contour or simpler solution.
	// 		If we only have one contour, do:
	// 		erode 12x12
	// 		MORP_OPEN 5x1
	//		Find contours again.

	if (ret > 0.0)
	{
		dir = catcierge_haar_guess_direction(ctx, img);

		printf("Direction: ");
		switch (dir)
		{
			case MATCH_DIR_IN: printf("IN"); break;
			case MATCH_DIR_OUT: printf("OUT"); break;
			default: printf("Unknown"); break;
		}
	}
fail:
	cvReleaseImage(&img_eq);
	if (tmp)
	{
		cvReleaseImage(&tmp);
	}

	return ret;
}

void catcierge_haar_matcher_usage()
{
	fprintf(stderr, " --cascade <path>       Path to the haar cascade xml generated by opencv_traincascade.\n");
	fprintf(stderr, " --min_size <WxH>       The size of the minimum\n");
	fprintf(stderr, "\n");
}

int catcierge_haar_matcher_parse_args(catcierge_haar_matcher_args_t *args, const char *key, char **values, size_t value_count)
{
	printf("Parse: %s %s", key, values[0]);
	if (!strcmp(key, "cascade"))
	{
		if (value_count == 1)
		{
			args->cascade = values[0];
		}
		else
		{
			fprintf(stderr, "Missing value for --cascade\n");
			return -1;
		}

		return 0;
	}

	if (!strcmp(key, "min_size"))
	{
		if (value_count == 1)
		{
			if (sscanf(values[0], "%dx%d", &args->min_width, &args->min_height) == EOF)
			{
				fprintf(stderr, "Invalid format for --min_size \"%s\"\n", values[0]);
				return -1;
			}
		}
		else
		{
			fprintf(stderr, "Missing value for --min_size\n");
			return -1;
		}

		return 0;
	}

	return 1;
}

void catcierge_haar_matcher_print_settings(catcierge_haar_matcher_args_t *args)
{
	assert(args);
	printf("Haar Cascade Matcher:\n");
	printf("    Cascade: %s\n", args->cascade);
	printf("   Min size: %dx%d\n", args->min_width, args->min_height);
	printf("\n");
}

void catcierge_haar_matcher_args_init(catcierge_haar_matcher_args_t *args)
{
	assert(args);
	memset(args, 0, sizeof(catcierge_haar_matcher_args_t));
	args->min_width = 80;
	args->min_height = 80;
	args->in_direction = DIR_RIGHT;
}
