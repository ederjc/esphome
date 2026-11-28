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

ZTEST(pdm_v1, test_nonblocking_read_and_borrowed_metadata_survive_reset)
{
	configure_and_start();
	inject_pairs(4, 13, 1013);
	void *buffer;
	size_t bytes;

	zassert_equal(pdm_v1_read(&device, 0, &buffer, &bytes, 10), -ENOTSUP);
	zassert_is_null(buffer);
	zassert_equal(bytes, 0);
	zassert_equal(pdm_v1_read(&device, 0, &buffer, &bytes, SYS_FOREVER_MS), -ENOTSUP);
	zassert_ok(pdm_v1_read(&device, 0, &buffer, &bytes, 0));
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
