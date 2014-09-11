#include "trickle.h"
#include "trickle_common.h"
#include "app_timer.h"

#include "boards.h"
#include "nrf51_bitfields.h"

#define MAX_TRICKLE_INSTANCES 8

#define APP_TIMER_PRESCALER 16

#define SYNC_TIMER_INTERVAL_MS      (10)

#define TRICKLE_FLAGS_T_DONE_Pos    (0)
#define TRICKLE_FLAGS_DISCARDED_Pos (1)

uint8_t rng_vals[64];
uint8_t rng_index;
uint8_t is_synced;

app_timer_id_t sync_timer_id;

/* pointer to next synchronized trickle instance to be executed */
trickle_t* next_trickle;

static void sync_timer_timeout_handler(void* unused)
{
    uint32_t start_cnt;
    APP_ERROR_CHECK(app_timer_cnt_get(&start_cnt));
    TICK_PIN(PIN_SYNC_TIME);
    if (next_trickle != NULL)
        next_trickle->tx_cb();
    next_trickle = NULL;
    
    APP_ERROR_CHECK(app_timer_start(sync_timer_id, APP_TIMER_TICKS(SYNC_TIMER_INTERVAL_MS, APP_TIMER_PRESCALER), NULL));
}



static void trickle_timeout_handler(void * trickle_ptr)
{
    trickle_t* trickle = (trickle_t*) trickle_ptr;
    SET_PIN(PIN_CPU_IN_USE);
    
    /* check which timer we just triggered */
    if (trickle->trickle_flags & (1 << TRICKLE_FLAGS_T_DONE_Pos)) /* interval timer */
    {
        /* double value of i */
        trickle->i = (trickle->i * 2 < trickle->i_max * trickle->i_min)?
                        trickle->i * 2 : 
                        trickle->i_max * trickle->i_min;
        
        trickle_interval_begin(trickle);
    }
    else /* t (tx timer) */
    {
        trickle->trickle_flags |= (1 << TRICKLE_FLAGS_T_DONE_Pos);
        
        if (trickle->c < trickle->k)
        {
            /* only schedule tx if this trickle message is fresher (lower i) than the already scheduled one */
            if (next_trickle == NULL || next_trickle->i > trickle->i)
                next_trickle = trickle;
            //trickle->tx_cb();
        }
        
        if (APP_TIMER_TICKS((trickle->i - trickle->t), APP_TIMER_PRESCALER) > 5 && trickle->i > trickle->t)
        {
            APP_ERROR_CHECK(app_timer_start(trickle->timer_id, APP_TIMER_TICKS((trickle->i - trickle->t), APP_TIMER_PRESCALER), trickle));
        }
        else
        {
            APP_ERROR_CHECK(app_timer_start(trickle->timer_id, APP_TIMER_TICKS(5, APP_TIMER_PRESCALER), trickle)); /* min tick count is 5 */
        }
    }
    CLEAR_PIN(PIN_CPU_IN_USE);
}


void trickle_setup(void)
{
    APP_TIMER_INIT(APP_TIMER_PRESCALER, MAX_TRICKLE_INSTANCES + 1, MAX_TRICKLE_INSTANCES + 1, false);   
    
    APP_ERROR_CHECK(app_timer_create(&sync_timer_id, APP_TIMER_MODE_SINGLE_SHOT, sync_timer_timeout_handler));
    
    is_synced = 0;
    /* Fill rng pool */
    for (uint8_t i = 0; i < 64; ++i)
    {
        NRF_RNG->EVENTS_VALRDY = 0;
        NRF_RNG->TASKS_START = 1;
        while (!NRF_RNG->EVENTS_VALRDY);
        rng_vals[i] = NRF_RNG->VALUE;
    } 
    rng_index = 0;
}

void trickle_init(trickle_t* trickle)
{
	uint32_t error = app_timer_create(&trickle->timer_id, APP_TIMER_MODE_SINGLE_SHOT, trickle_timeout_handler);
    APP_ERROR_CHECK(error);
    
    trickle_interval_begin(trickle);
}

void trickle_interval_begin(trickle_t* trickle)
{
    /* if the system hasn't synced up with anyone else, just start own sync up */
    if (is_synced == 0 && trickle->c == 0)
    {
        APP_ERROR_CHECK(app_timer_start(sync_timer_id, APP_TIMER_TICKS(SYNC_TIMER_INTERVAL_MS, APP_TIMER_PRESCALER), NULL));
    }
        
    trickle->c = 0;
    
    uint32_t rand_number =  ((uint32_t) rng_vals[(rng_index++) & 0x3F])       |
                            ((uint32_t) rng_vals[(rng_index++) & 0x3F]) << 8  |
                            ((uint32_t) rng_vals[(rng_index++) & 0x3F]) << 16 |
                            ((uint32_t) rng_vals[(rng_index++) & 0x3F]) << 24;
    
    uint32_t i_half = trickle->i / 2;
    trickle->t = i_half + (rand_number % i_half);
    
    trickle->trickle_flags &= ~(1 << TRICKLE_FLAGS_T_DONE_Pos);

    APP_ERROR_CHECK(app_timer_start(trickle->timer_id, APP_TIMER_TICKS(trickle->t, APP_TIMER_PRESCALER), trickle));
    
    TICK_PIN(PIN_NEW_INTERVAL);
    TICK_PIN((PIN_INT0 + trickle->id));
}

void trickle_rx_consistent(trickle_t* trickle)
{
    ++trickle->c;
}

void trickle_rx_inconsistent(trickle_t* trickle)
{
    if (trickle->i > trickle->i_min)
    {
        trickle_timer_reset(trickle);
    }
}

void trickle_timer_reset(trickle_t* trickle)
{
    trickle->trickle_flags &= ~(1 << TRICKLE_FLAGS_T_DONE_Pos);
    APP_ERROR_CHECK(app_timer_stop(trickle->timer_id));
    trickle->i = trickle->i_min; 
    trickle_interval_begin(trickle);
}

void trickle_sync(void)
{
    //if (!is_synced)
    {
        //APP_ERROR_CHECK(app_timer_stop(sync_timer_id));
        APP_ERROR_CHECK(app_timer_start(sync_timer_id, APP_TIMER_TICKS(SYNC_TIMER_INTERVAL_MS, APP_TIMER_PRESCALER), NULL));
        is_synced = 1;
    }
}
