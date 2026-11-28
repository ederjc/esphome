#include "../drivers/audio/dmic_infineon_pdm_v1.c"
#include <zephyr/ztest.h>

K_MEM_SLAB_DEFINE(test_slab, 16, 4, 4);
static PDM_Type registers;
static struct pdm_v1_data data;
static void connect_irq(const struct device *dev) { ARG_UNUSED(dev); }
static const struct pdm_v1_config config = {.base = &registers, .connect_irq = connect_irq};
static struct device_state device_state = {.initialized = true};
static const struct device device = {
    .config = &config, .data = &data, .api = &pdm_v1_api, .state = &device_state, .name = "pdm_test"
};

static struct dmic_cfg request(struct pcm_stream_cfg *stream)
{
	struct dmic_cfg result = {
	    .streams = stream,
	    .io = {.min_pdm_clk_freq = 1000000,
		   .max_pdm_clk_freq = 3000000,
		   .min_pdm_clk_dc = 40,
		   .max_pdm_clk_dc = 60},
	    .channel = {.req_num_streams = 1, .req_num_chan = 2, .req_chan_map_lo = 0x10},
	};
	return result;
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	if (data.state != DMIC_STATE_UNINIT) {
		pdm_v1_trigger(&device, DMIC_TRIGGER_RESET);
	}
	memset(&registers, 0, sizeof(registers));
	memset(&data, 0, sizeof(data));
	zassert_ok(pdm_v1_init(&device));
	zassert_equal(data.state, DMIC_STATE_INITIALIZED);
	fake_hf1 = 8000000;
	fake_pdm_init_result = CY_PDM_PCM_SUCCESS;
}

static void inject_pairs(unsigned int frames, uint16_t left, uint16_t right)
{
	registers.count = registers.position = 0;
	for (unsigned int i = 0; i < frames; i++) {
		registers.words[registers.count++] = left + i;
		registers.words[registers.count++] = right + i;
	}
	registers.status = CY_PDM_PCM_INTR_RX_TRIGGER;
	pdm_v1_isr(&device);
}

static void configure_and_start(void)
{
	struct pcm_stream_cfg stream = {
	    .pcm_rate = 15625,
	    .pcm_width = 16,
	    .block_size = 16,
	    .mem_slab = &test_slab,
	};
	struct dmic_cfg cfg = request(&stream);

	zassert_ok(pdm_v1_configure(&device, &cfg));
	zassert_equal(cfg.channel.act_num_chan, 2);
	zassert_equal(cfg.channel.act_chan_map_lo, 0x10);
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_START));
	inject_pairs(64, 0, 0); /* startup discarded */
}

ZTEST(pdm_v1, test_stereo_shape_and_restart_returns_all_slabs)
{
	configure_and_start();
	inject_pairs(4, 1, 1001);
	void *buffer;
	size_t bytes;

	zassert_ok(pdm_v1_read(&device, 0, &buffer, &bytes, 0));
	zassert_equal(bytes, 16);
	const uint16_t expected[] = {1, 1001, 2, 1002, 3, 1003, 4, 1004};

	zassert_mem_equal(buffer, expected, sizeof(expected));
	inject_pairs(6, 10, 1010); /* one queued block and one partial */
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_STOP));
	zassert_equal(k_mem_slab_num_used_get(&test_slab), 1);
	zassert_mem_equal(buffer, expected, sizeof(expected));
	k_mem_slab_free(&test_slab, buffer);
	zassert_equal(k_mem_slab_num_used_get(&test_slab), 0);
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_START));
	inject_pairs(64, 0, 0);
	inject_pairs(4, 42, 1042);
	zassert_ok(pdm_v1_read(&device, 0, &buffer, &bytes, 0));
	zassert_equal(((uint16_t *)buffer)[0], 42);
	zassert_equal(((uint16_t *)buffer)[1], 1042);
	k_mem_slab_free(&test_slab, buffer);
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_STOP));
	zassert_equal(k_mem_slab_num_used_get(&test_slab), 0);
}

ZTEST(pdm_v1, test_bad_clock_and_format_never_start)
{
	struct pcm_stream_cfg stream = {
	    .pcm_rate = 15625,
	    .pcm_width = 16,
	    .block_size = 16,
	    .mem_slab = &test_slab,
	};
	struct dmic_cfg cfg = request(&stream);

	fake_hf1 = 100000000;
	zassert_equal(pdm_v1_configure(&device, &cfg), -EIO);
	fake_hf1 = 8000000;
	cfg.channel.req_num_chan = 1;
	zassert_equal(pdm_v1_configure(&device, &cfg), -EINVAL);
	cfg.channel.req_num_chan = 2;
	stream.block_size = 18;
	zassert_equal(pdm_v1_configure(&device, &cfg), -EINVAL);
	zassert_false(registers.enabled);
	zassert_equal(k_mem_slab_num_used_get(&test_slab), 0);
}

ZTEST(pdm_v1, test_exhaustion_is_error_until_stop_and_restart)
{
	configure_and_start();
	inject_pairs(16, 2, 1002);
	void *buffer;
	size_t bytes;

	zassert_equal(pdm_v1_read(&device, 0, &buffer, &bytes, 0), -EIO);
	zassert_false(registers.enabled);
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_STOP));
	zassert_equal(k_mem_slab_num_used_get(&test_slab), 0);
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_START));
	inject_pairs(64, 0, 0);
	inject_pairs(4, 91, 2091);
	zassert_ok(pdm_v1_read(&device, 0, &buffer, &bytes, 0));
	const uint16_t expected[] = {91, 2091, 92, 2092, 93, 2093, 94, 2094};
	zassert_equal(bytes, sizeof(expected));
	zassert_mem_equal(buffer, expected, sizeof(expected));
	k_mem_slab_free(&test_slab, buffer);
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_STOP));
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_RESET));
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_STOP));
	zassert_equal(pdm_v1_trigger(&device, DMIC_TRIGGER_START), -EIO);
}

ZTEST(pdm_v1, test_read_timeouts_and_borrowed_metadata_survive_reset)
{
	configure_and_start();
	void *buffer;
	size_t bytes;

	zassert_equal(pdm_v1_read(&device, 0, &buffer, &bytes, 0), -ENOMSG);
	zassert_equal(pdm_v1_read(&device, 0, &buffer, &bytes, 10), -EAGAIN);
	zassert_is_null(buffer);
	zassert_equal(bytes, 0);
	inject_pairs(4, 13, 1013);
	zassert_ok(pdm_v1_read(&device, 0, &buffer, &bytes, SYS_FOREVER_MS));
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_RESET));
	zassert_equal(bytes, 16);
	zassert_equal(((uint16_t *)buffer)[0], 13);
	zassert_equal(((uint16_t *)buffer)[1], 1013);
	zassert_equal(k_mem_slab_num_used_get(&test_slab), 1);
	k_mem_slab_free(&test_slab, buffer);
	zassert_equal(k_mem_slab_num_used_get(&test_slab), 0);
	zassert_equal(pdm_v1_read(&device, 0, &buffer, &bytes, 0), -EIO);
	zassert_is_null(buffer);
	zassert_equal(bytes, 0);
}

ZTEST(pdm_v1, test_failed_pdl_init_drops_previous_configuration)
{
	struct pcm_stream_cfg stream = {
	    .pcm_rate = 15625,
	    .pcm_width = 16,
	    .block_size = 16,
	    .mem_slab = &test_slab,
	};
	struct dmic_cfg cfg = request(&stream);

	configure_and_start();
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_STOP));
	zassert_equal(data.state, DMIC_STATE_CONFIGURED);
	fake_pdm_init_result = CY_PDM_PCM_BAD_PARAM;
	zassert_equal(pdm_v1_configure(&device, &cfg), -EIO);
	zassert_equal(data.state, DMIC_STATE_INITIALIZED);
	zassert_is_null(data.slab);
	zassert_equal(data.block_size, 0);
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_STOP));
	zassert_equal(data.state, DMIC_STATE_INITIALIZED);
	zassert_equal(pdm_v1_trigger(&device, DMIC_TRIGGER_START), -EIO);
	zassert_false(registers.enabled);
	zassert_equal(k_mem_slab_num_used_get(&test_slab), 0);
}

/* The reader is lower priority than the cooperative ztest thread, so it only
 * runs, and blocks in dmic_read(), while the test thread sleeps.
 */
K_THREAD_STACK_DEFINE(reader_stack, 1024);
static struct k_thread reader_thread;
static volatile int reader_ret;
static void *reader_buffer;
static size_t reader_bytes;

static void reader_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	reader_ret = pdm_v1_read(&device, 0, &reader_buffer, &reader_bytes, SYS_FOREVER_MS);
}

static void start_blocked_reader(void)
{
	reader_ret = 1;
	reader_buffer = (void *)&reader_ret;
	reader_bytes = 1;
	k_thread_create(&reader_thread, reader_stack, K_THREAD_STACK_SIZEOF(reader_stack),
			reader_entry, NULL, NULL, NULL, K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
	k_msleep(20);
	zassert_equal(reader_ret, 1, "reader must still be blocked");
	zassert_equal(data.readers, 1);
}

static int join_reader(void)
{
	zassert_ok(k_thread_join(&reader_thread, K_MSEC(1000)));
	zassert_equal(data.readers, 0);
	return reader_ret;
}

ZTEST(pdm_v1, test_capture_error_wakes_forever_reader_with_eio)
{
	configure_and_start();
	start_blocked_reader();
	registers.status = CY_PDM_PCM_INTR_RX_OVERFLOW;
	pdm_v1_isr(&device);
	zassert_equal(data.state, DMIC_STATE_ERROR);
	zassert_equal(join_reader(), -EIO);
	zassert_is_null(reader_buffer);
	zassert_equal(reader_bytes, 0);
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_STOP));
	zassert_equal(k_mem_slab_num_used_get(&test_slab), 0);
	/* START purges the unread STOP sentinel; an empty queue is not an error. */
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_START));
	void *buffer;
	size_t bytes;

	zassert_equal(pdm_v1_read(&device, 0, &buffer, &bytes, 0), -ENOMSG);
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_STOP));
}

ZTEST(pdm_v1, test_stop_wakes_reader_and_blocks_reconfigure_until_it_returns)
{
	struct pcm_stream_cfg stream = {
	    .pcm_rate = 15625,
	    .pcm_width = 16,
	    .block_size = 8,
	    .mem_slab = &test_slab,
	};
	struct dmic_cfg cfg = request(&stream);

	configure_and_start();
	start_blocked_reader();
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_STOP));
	/* The woken reader has not run yet; its 16-byte snapshot must stay valid. */
	zassert_equal(pdm_v1_configure(&device, &cfg), -EBUSY);
	zassert_equal(data.block_size, 16);
	zassert_equal(join_reader(), -EIO);
	zassert_is_null(reader_buffer);
	zassert_equal(reader_bytes, 0);
	zassert_equal(k_mem_slab_num_used_get(&test_slab), 0);
	zassert_ok(pdm_v1_configure(&device, &cfg));
	zassert_equal(data.block_size, 8);
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_RESET));
}

ZTEST(pdm_v1, test_forever_reader_gets_real_block_with_its_size_never_sentinel)
{
	configure_and_start();
	start_blocked_reader();
	inject_pairs(4, 21, 1021);
	zassert_equal(join_reader(), 0);
	zassert_not_null(reader_buffer);
	zassert_equal(reader_bytes, 16);
	const uint16_t expected[] = {21, 1021, 22, 1022, 23, 1023, 24, 1024};

	zassert_mem_equal(reader_buffer, expected, sizeof(expected));
	k_mem_slab_free(&test_slab, reader_buffer);
	start_blocked_reader();
	zassert_ok(pdm_v1_trigger(&device, DMIC_TRIGGER_RESET));
	zassert_equal(join_reader(), -EIO);
	zassert_is_null(reader_buffer);
	zassert_equal(k_mem_slab_num_used_get(&test_slab), 0);
}

const struct device *pdm_test_reset(void)
{
	before(NULL);
	return &device;
}

void pdm_test_inject(unsigned int frames, uint16_t left, uint16_t right)
{
	inject_pairs(frames, left, right);
}

ZTEST_SUITE(pdm_v1, NULL, NULL, before, NULL, NULL);
