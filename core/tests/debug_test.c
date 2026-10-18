/* The debugger engine: a little program poked into RAM, then break, step,
   step over and out, frame step, watchpoints, conditions, a scanline break,
   a CIM stop (attached and not), the A assembler, labels, the budget,
   snapshots and the SIO trace. */
#include "test_util.h"
#include "test_files.h"
#include "a800debug.h"

/* $0600 LDA #$05 / JSR $0610 / STA $0700 / LDA $0701 / INX / JMP $0600
   $0610 CLC / ADC #$01 / RTS                     $0620 $02 (a CIM) */
static const uint8_t prog[] = {
    0xA9, 0x05, 0x20, 0x10, 0x06, 0x8D, 0x00, 0x07, 0xAD, 0x01, 0x07, 0xE8, 0x4C, 0x00, 0x06,
    0xEA, 0x18, 0x69, 0x01, 0x60};

static a800debug_cpu cpu(a800debug *d)
{
    a800debug_cpu c;
    a800debug_cpu_get(d, &c);
    return c;
}

/* Waits for a stop; its reason into why. */
static int wait_stop(a800debug *d, char *why, int whysz)
{
    int t;
    for (t = 0; t < 3000 && !a800debug_is_stopped(d); t += 5)
        test_sleep_ms(5);
    a800debug_stop_reason(d, why, whysz, NULL);
    return a800debug_is_stopped(d);
}

/* Parks the machine in the program: PC, NMIs and IRQs off. */
static void enter_prog(a800debug *d, int pc)
{
    a800debug_write(d, 0xD40E, 0x00);   /* NMIEN */
    a800debug_cpu_set(d, A800_FLAG_I, 1);
    a800debug_cpu_set(d, A800_REG_PC, pc);
}

int main(void)
{
    a800session *s = test_session("debug");
    a800debug *d;
    char why[128], out[8192], msg[256], atr[1100];
    uint8_t b[4];
    int i, id, x0, max, rc;
    a800debug_line lines[8];
    a800debug_history hist[16];
    a800debug_sio sio[64];

    test_set_pref(s, "basic", 1);
    test_start(s, 0);
    CHECK(test_wait_text(s, "Ready", 8000), "BASIC");
    d = a800session_debugger(s);

    /* attach: stops */
    a800debug_attach(d);
    CHECK(wait_stop(d, why, sizeof why), "attach stops");
    CHECK(!strcmp(why, "Paused"), "reason %s", why);
    for (i = 0; i < (int)sizeof prog; i++)
        a800debug_write(d, (uint16_t)(0x0600 + i), prog[i]);
    a800debug_write(d, 0x0620, 0x02);
    enter_prog(d, 0x0600);

    /* step, step over a JSR, step into it and out */
    a800debug_step(d);
    CHECK(wait_stop(d, why, sizeof why), "step");
    CHECK(cpu(d).pc == 0x0602 && cpu(d).a == 5 && !strcmp(why, "Step"), "step: %04X %s", cpu(d).pc, why);
    a800debug_step_over(d);
    CHECK(wait_stop(d, why, sizeof why), "over");
    CHECK(cpu(d).pc == 0x0605 && cpu(d).a == 6, "over: %04X A=%02X %s", cpu(d).pc, cpu(d).a, why);
    a800debug_step(d);
    CHECK(wait_stop(d, why, sizeof why) && cpu(d).pc == 0x0608, "sta");
    a800debug_read(d, 0x0700, b, 1);
    CHECK(b[0] == 6, "STA wrote %02X", b[0]);
    a800debug_cpu_set(d, A800_REG_PC, 0x0602);
    a800debug_step(d);
    CHECK(wait_stop(d, why, sizeof why) && cpu(d).pc == 0x0610, "into JSR: %04X", cpu(d).pc);
    a800debug_step_out(d);
    CHECK(wait_stop(d, why, sizeof why), "out");
    CHECK(cpu(d).pc == 0x0605 && !strcmp(why, "Returned"), "out: %04X %s", cpu(d).pc, why);

    /* an execution breakpoint, hit twice: resuming steps off it */
    id = a800debug_breakpoint_add(d, A800DEBUG_BP_EXEC, 0x060B, 0x060B, "", msg, sizeof msg);
    CHECK(id == 1, "bp id %d %s", id, msg);
    CHECK(a800debug_breakpoint_check(d, 0x060B) == 1, "check");
    a800debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why), "bp");
    CHECK(cpu(d).pc == 0x060B && !strcmp(why, "Breakpoint 1"), "bp: %04X %s", cpu(d).pc, why);
    x0 = cpu(d).x;
    a800debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why), "bp again");
    CHECK(cpu(d).pc == 0x060B && cpu(d).x == ((x0 + 1) & 0xff), "bp again: %04X X=%02X (%02X)",
          cpu(d).pc, cpu(d).x, x0);
    a800debug_disassemble(d, 0x060B, lines, 1, NULL);
    CHECK(lines[0].has_breakpoint && lines[0].is_pc && strstr(lines[0].disasm, "INX"),
          "line: %s", lines[0].disasm);
    a800debug_breakpoint_toggle(d, 0x060B);
    CHECK(a800debug_breakpoint_check(d, 0x060B) == 0, "toggled off");

    /* watchpoints: a write, then a read */
    id = a800debug_breakpoint_add(d, A800DEBUG_BP_WRITE, 0x0700, 0x0700, "", msg, sizeof msg);
    CHECK(id == 1001, "wp id %d", id);
    a800debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why), "write wp");
    CHECK(cpu(d).pc == 0x0605 && strstr(why, "Watchpoint 1001: write $0700"), "write wp: %04X %s",
          cpu(d).pc, why);
    a800debug_breakpoint_remove(d, id);
    id = a800debug_breakpoint_add(d, A800DEBUG_BP_READ, 0x0701, 0x0701, "", msg, sizeof msg);
    a800debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why), "read wp");
    CHECK(cpu(d).pc == 0x0608 && strstr(why, "read $0701"), "read wp: %04X %s", cpu(d).pc, why);
    a800debug_breakpoint_clear(d);

    /* a condition */
    CHECK(a800debug_breakpoint_add(d, A800DEBUG_BP_EXEC, 0x060B, 0x060B, "X=", msg, sizeof msg) < 0,
          "bad condition");
    a800debug_cpu_set(d, A800_REG_X, 0);
    id = a800debug_breakpoint_add(d, A800DEBUG_BP_EXEC, 0x060B, 0x060B, "X=$10 && MEM[$0700]=6", msg, sizeof msg);
    CHECK(id > 0, "cond add: %s", msg);
    a800debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why), "cond");
    CHECK(cpu(d).pc == 0x060B && cpu(d).x == 0x10, "cond: %04X X=%02X", cpu(d).pc, cpu(d).x);
    a800debug_breakpoint_clear(d);

    /* history: the loop just ran */
    rc = a800debug_history_get(d, hist, 16);
    CHECK(rc == 16, "history %d", rc);
    for (i = 13; i < 16 && !(hist[i].pc == 0x0608 && strstr(hist[i].disasm, "LDA")); i++)
        ;
    CHECK(i < 16, "history: %04X %s / %04X %s", hist[14].pc, hist[14].disasm, hist[15].pc,
          hist[15].disasm);

    /* frame steps */
    a800debug_frame(d);
    CHECK(wait_stop(d, why, sizeof why), "frame");
    {
        a800debug_cpu c = cpu(d), c2;
        CHECK(c.scanline == 248 && !strcmp(why, "Frame"), "frame: line %d %s", c.scanline, why);
        a800debug_frame(d);
        CHECK(wait_stop(d, why, sizeof why), "frame 2");
        c2 = cpu(d);
        CHECK(c2.scanline == 248 && c2.frame == c.frame + 1, "frame 2: line %d frame %u (%u)",
              c2.scanline, c2.frame, c.frame);
    }

    /* the console: a scanline break, CONT */
    rc = a800debug_command(d, "BLINE 100", out, sizeof out);
    CHECK(rc == A800DBG_CMD_OK && strstr(out, "scanline 100"), "BLINE: %s", out);
    rc = a800debug_command(d, "CONT", out, sizeof out);
    CHECK(rc == A800DBG_CMD_RESUMED, "CONT %d", rc);
    CHECK(wait_stop(d, why, sizeof why), "scanline");
    CHECK(cpu(d).scanline == 100 && !strcmp(why, "Scanline 100"), "scanline: %d %s", cpu(d).scanline, why);

    /* the A assembler asks for lines */
    rc = a800debug_command(d, "A 0640", out, sizeof out);
    CHECK(rc == A800DBG_CMD_NEEDS_INPUT && strstr(out, "0640: "), "A: %d %s", rc, out);
    {
        a800debug_cpu c = cpu(d);   /* inspection works meanwhile */
        CHECK(c.scanline == 100, "cpu while assembling");
    }
    rc = a800debug_command(d, "LDA #$42", out, sizeof out);
    CHECK(rc == A800DBG_CMD_NEEDS_INPUT && strstr(out, "0642: "), "A line: %d %s", rc, out);
    rc = a800debug_command(d, "", out, sizeof out);
    CHECK(rc == A800DBG_CMD_OK, "A end %d", rc);
    a800debug_read(d, 0x0640, b, 2);
    CHECK(b[0] == 0xA9 && b[1] == 0x42, "assembled %02X %02X", b[0], b[1]);

    /* labels: the OS's, and a symbol file */
    CHECK(a800debug_label_address(d, "CIOV") == 0xE456, "CIOV");
    a800debug_address_label(d, 0xE456, why, sizeof why);
    CHECK(!strcmp(why, "CIOV"), "label %s", why);
    a800debug_write(d, 0x0650, 0x20);
    a800debug_write(d, 0x0651, 0x56);
    a800debug_write(d, 0x0652, 0xE4);
    a800debug_disassemble(d, 0x0650, lines, 1, NULL);
    CHECK(strstr(lines[0].comment, "CIOV") && strstr(lines[0].disasm, "JSR $E456")
          && !strcmp(lines[0].bytes, "20 56 E4"), "dis: [%s] [%s] [%s]", lines[0].bytes, lines[0].disasm,
          lines[0].comment);
    {
        char lab[1100];
        FILE *fp;
        snprintf(lab, sizeof lab, "%s/game.lbl", a800session_data_path(s));
        fp = fopen(lab, "w");
        CHECK(fp != NULL, "lbl");
        fputs("al 000600 .main\nal C:0610 .addone\n", fp);
        fclose(fp);
        CHECK(a800debug_load_symbols(d, lab, msg, sizeof msg) == 2, "symbols: %s", msg);
        CHECK(a800debug_label_address(d, "addone") == 0x0610, "addone");
        a800debug_disassemble(d, 0x0602, lines, 1, NULL);
        CHECK(strstr(lines[0].comment, "addone"), "dis jsr: %s ;%s", lines[0].disasm, lines[0].comment);
        CHECK(a800debug_label_address(d, "CIOV") == 0xE456, "OS labels kept");
    }

    /* the budget: 10 breakpoints take 19 entries; an 11th does not fit */
    for (i = 0; i < 10; i++)
        CHECK(a800debug_breakpoint_add(d, A800DEBUG_BP_EXEC, (uint16_t)(0x5000 + i), (uint16_t)(0x5000 + i),
                                       "", msg, sizeof msg) > 0, "bp %d: %s", i, msg);
    CHECK(a800debug_breakpoint_budget(d, &max) == 19 && max == 20, "budget");
    CHECK(a800debug_breakpoint_add(d, A800DEBUG_BP_EXEC, 0x5100, 0x5100, "", msg, sizeof msg) < 0
          && strstr(msg, "20"), "over budget: %s", msg);
    a800debug_breakpoint_clear(d);

    /* snapshots */
    enter_prog(d, 0x0600);
    CHECK(a800debug_snapshot_save(d, 0) == 0 && a800debug_snapshot_exists(d, 0), "snapshot");
    a800debug_step(d);
    CHECK(wait_stop(d, why, sizeof why), "step");
    a800debug_step(d);
    CHECK(wait_stop(d, why, sizeof why) && cpu(d).pc == 0x0610, "steps");
    CHECK(a800debug_snapshot_restore(d, 0) == 0, "restore");
    CHECK(cpu(d).pc == 0x0600, "restored pc %04X", cpu(d).pc);

    /* chips */
    {
        a800debug_antic an;
        a800debug_map map;
        a800debug_antic_get(d, &an);
        CHECK(an.dlist != 0 && an.dl_dma, "antic dlist %04X dmactl %02X", an.dlist, an.dmactl);
        a800debug_dlist_text(d, out, sizeof out);
        CHECK(strstr(out, "JVB") && strstr(out, "LMS") && strstr(out, "MODE 2"), "dlist:\n%s", out);
        a800debug_map_get(d, &map);
        CHECK(map.xl && map.ram_kb == 128 && map.basic_rom, "map xl=%d ram=%d basic=%d", map.xl, map.ram_kb,
              map.basic_rom);
    }

    /* stopped at a breakpoint, Power Cycle (a frame job) must still run:
       the breakpoint would otherwise fire for ever as the park lets go */
    enter_prog(d, 0x0600);
    id = a800debug_breakpoint_add(d, A800DEBUG_BP_EXEC, 0x0605, 0x0605, "", msg, sizeof msg);
    a800debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why) && cpu(d).pc == 0x0605, "bp before power cycle");
    CHECK(a800session_power_cycle(s) == 0, "power cycle at a breakpoint");
    CHECK(test_wait_text(s, "Ready", 8000), "BASIC after a power cycle at a breakpoint");
    a800debug_breakpoint_clear(d);
    a800debug_stop(d);
    CHECK(wait_stop(d, why, sizeof why), "stopped again");
    for (i = 0; i < (int)sizeof prog; i++)
        a800debug_write(d, (uint16_t)(0x0600 + i), prog[i]);
    a800debug_write(d, 0x0620, 0x02);

    /* a CIM, attached: a stop */
    a800debug_cpu_set(d, A800_REG_PC, 0x0620);
    a800debug_resume(d);
    CHECK(wait_stop(d, why, sizeof why), "cim");
    CHECK(!strncmp(why, "CIM", 3), "cim: %s", why);

    /* ...and detached: parked, with a notice; Power Cycle gets it going */
    a800debug_cpu_set(d, A800_REG_PC, 0x0620);
    a800debug_detach(d);
    for (i = 0; i < 3000 && !a800debug_is_stopped(d); i += 5)
        test_sleep_ms(5);
    CHECK(a800debug_is_stopped(d), "detached cim parks");
    CHECK(a800session_take_notice(s, msg, sizeof msg) > 0 && strstr(msg, "crashed"), "notice: %s", msg);
    CHECK(a800session_power_cycle(s) == 0, "power cycle");
    CHECK(test_wait_text(s, "Ready", 8000), "BASIC after the crash");
    CHECK(!a800debug_is_stopped(d), "running");

    /* the SIO trace: boot a disk with the debugger attached */
    snprintf(atr, sizeof atr, "%s/hello.atr", a800session_data_path(s));
    CHECK(write_hello_atr(atr) == 0, "atr");
    a800debug_attach(d);
    CHECK(wait_stop(d, why, sizeof why), "attach again");
    a800debug_sio_clear(d);
    a800debug_resume(d);
    CHECK(a800session_open(s, atr, msg, sizeof msg) == 0, "open: %s", msg);
    CHECK(test_wait_text(s, "HELLO", 8000), "boots");
    rc = a800debug_sio_trace(d, sio, 64);
    CHECK(rc > 0, "sio trace empty");
    for (i = 0; i < rc && !strstr(sio[i].text, "D1: READ SECTOR 1"); i++)
        ;
    CHECK(i < rc, "no sector 1 read (first: %s %s)", sio[0].path, sio[0].text);
    a800debug_detach(d);

    /* quitting with the PC on an execution breakpoint must not hang */
    a800debug_attach(d);
    CHECK(wait_stop(d, why, sizeof why), "attach before quitting");
    enter_prog(d, 0x0600);
    for (i = 0; i < (int)sizeof prog; i++)
        a800debug_write(d, (uint16_t)(0x0600 + i), prog[i]);
    a800debug_breakpoint_add(d, A800DEBUG_BP_EXEC, 0x0600, 0x0600, "", msg, sizeof msg);
    a800session_free(s);
    printf("debug ok\n");
    return 0;
}
