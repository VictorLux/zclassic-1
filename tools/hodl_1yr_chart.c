/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Generate SVG chart: % of ZCL supply unmoved for 1+ year over past 4 years.
 * Uses zclassic-cli for data. Outputs SVG to stdout. Pure C23, no Python. */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* Simple JSON field extractor — no library needed */
static double json_field_double(const char *json, const char *field)
{
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\":", field);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '"') p++;
    return atof(p);
}

static double json_bucket_zcl(const char *json, const char *age_label)
{
    const char *p = json;
    while ((p = strstr(p, age_label)) != NULL) {
        const char *zcl = strstr(p, "\"zcl\":");
        if (!zcl || zcl - p > 200) { p++; continue; }
        zcl += 6;
        while (*zcl == ' ' || *zcl == '"') zcl++;
        return atof(zcl);
    }
    return 0;
}

int main(void)
{
    /* Get HODL wave data from node */
    FILE *fp = popen("./zclassic-cli gethodlwave 2>/dev/null", "r");
    if (!fp) { fprintf(stderr, "Cannot run zclassic-cli\n"); return 1; }
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    pclose(fp);
    buf[n] = 0;

    double total_supply = json_field_double(buf, "total_supply_zcl");
    if (total_supply < 1) { fprintf(stderr, "No data\n"); return 1; }

    /* Parse bucket ZCL amounts */
    double zcl_1d   = json_bucket_zcl(buf, "< 1 day");
    double zcl_1w   = json_bucket_zcl(buf, "1d - 1w");
    double zcl_1m   = json_bucket_zcl(buf, "1w - 1m");
    double zcl_3m   = json_bucket_zcl(buf, "1 - 3m");
    double zcl_6m   = json_bucket_zcl(buf, "3 - 6m");
    double zcl_12m  = json_bucket_zcl(buf, "6 - 12m");
    double zcl_2y   = json_bucket_zcl(buf, "1 - 2y");
    double zcl_3y   = json_bucket_zcl(buf, "2 - 3y");
    double zcl_5y   = json_bucket_zcl(buf, "3 - 5y");
    double zcl_old  = json_bucket_zcl(buf, "> 5y");

    /* Bucket boundaries in days and cumulative ZCL from oldest */
    struct { double days; double zcl; } buckets[] = {
        {    0, zcl_1d  },  /* < 1 day */
        {    1, zcl_1w  },  /* 1d-1w */
        {    7, zcl_1m  },  /* 1w-1m */
        {   30, zcl_3m  },  /* 1-3m */
        {   90, zcl_6m  },  /* 3-6m */
        {  180, zcl_12m },  /* 6-12m */
        {  365, zcl_2y  },  /* 1-2y */
        {  730, zcl_3y  },  /* 2-3y */
        { 1095, zcl_5y  },  /* 3-5y */
        { 1825, zcl_old },  /* >5y */
    };
    int nbuckets = 10;

    /* For each sample point (biweekly over 4 years), compute:
     * "what % of today's supply was 1+ year old at that historical moment?"
     *
     * A coin currently X days old was (X - delta) days old delta days ago.
     * So at time (now - delta), the "1yr old" threshold catches coins
     * that are currently at least (365 + delta) days old. */

    time_t now = time(NULL);
    int W = 900, H = 400;
    int PAD_L = 70, PAD_R = 30, PAD_T = 40, PAD_B = 50;
    int chart_w = W - PAD_L - PAD_R;
    int chart_h = H - PAD_T - PAD_B;

    int npoints = 104; /* biweekly for 4 years */
    double xs[104], ys[104];
    double four_years = 4 * 365.25;
    double y_min = 100, y_max = 0;

    for (int i = 0; i < npoints; i++) {
        double delta_days = four_years * (1.0 - (double)i / (npoints - 1));
        double threshold_days = 365.0 + delta_days;

        /* Sum all coins with current age >= threshold_days */
        double old_zcl = 0;
        double bucket_ends[] = { 1, 7, 30, 90, 180, 365, 730, 1095, 1825, 3650 };
        for (int b = 0; b < nbuckets; b++) {
            double bstart = buckets[b].days;
            double bend = bucket_ends[b];
            if (bstart >= threshold_days) {
                old_zcl += buckets[b].zcl;
            } else if (bend > threshold_days) {
                double frac = (bend - threshold_days) / (bend - bstart);
                old_zcl += buckets[b].zcl * frac;
            }
        }

        /* Approximate supply at historical point (linear growth model) */
        double supply_frac = 1.0 - delta_days / (9 * 365.25); /* ~9yr chain age */
        if (supply_frac < 0.5) supply_frac = 0.5;
        double est_supply = total_supply * supply_frac;

        double pct = (old_zcl / est_supply) * 100.0;
        if (pct > 100) pct = 100;
        if (pct < 0) pct = 0;

        xs[i] = PAD_L + chart_w * ((double)i / (npoints - 1));
        ys[i] = pct;
        if (pct < y_min) y_min = pct;
        if (pct > y_max) y_max = pct;
    }

    /* Y axis range with padding */
    double y_lo = floor(y_min / 10) * 10;
    double y_hi = ceil(y_max / 10) * 10;
    if (y_hi - y_lo < 20) y_hi = y_lo + 20;

    /* SVG output */
    printf("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    printf("<svg xmlns=\"http://www.w3.org/2000/svg\" "
           "viewBox=\"0 0 %d %d\" width=\"%d\" height=\"%d\">\n", W, H, W, H);
    printf("<style>text{font-family:monospace;fill:#333}"
           ".t{font-size:16px;font-weight:bold}"
           ".l{font-size:11px}.k{font-size:10px;fill:#666}</style>\n");
    printf("<rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n", W, H);

    /* Title */
    printf("<text x=\"%d\" y=\"24\" text-anchor=\"middle\" class=\"t\">"
           "%% of ZCL Supply Unmoved for 1+ Year</text>\n", W / 2);

    /* Y grid + labels */
    for (double v = y_lo; v <= y_hi; v += 10) {
        double y = PAD_T + chart_h * (1.0 - (v - y_lo) / (y_hi - y_lo));
        printf("<line x1=\"%d\" y1=\"%.0f\" x2=\"%d\" y2=\"%.0f\" "
               "stroke=\"#eee\" stroke-width=\"1\"/>\n",
               PAD_L, y, W - PAD_R, y);
        printf("<text x=\"%d\" y=\"%.0f\" text-anchor=\"end\" class=\"k\">"
               "%.0f%%</text>\n", PAD_L - 8, y + 4, v);
    }

    /* X axis: year labels */
    for (int yr = 0; yr <= 4; yr++) {
        double x = PAD_L + chart_w * ((double)yr / 4.0);
        time_t t = now - (time_t)((4 - yr) * 365.25 * 86400);
        struct tm *tm = localtime(&t);
        char label[32];
        strftime(label, sizeof(label), "%b %Y", tm);
        printf("<line x1=\"%.0f\" y1=\"%d\" x2=\"%.0f\" y2=\"%d\" "
               "stroke=\"#eee\" stroke-width=\"1\"/>\n",
               x, PAD_T, x, H - PAD_B);
        printf("<text x=\"%.0f\" y=\"%d\" text-anchor=\"middle\" class=\"k\">"
               "%s</text>\n", x, H - PAD_B + 18, label);
    }

    /* Axes */
    printf("<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#999\"/>\n",
           PAD_L, PAD_T, PAD_L, H - PAD_B);
    printf("<line x1=\"%d\" y1=\"%d\" x2=\"%d\" y2=\"%d\" stroke=\"#999\"/>\n",
           PAD_L, H - PAD_B, W - PAD_R, H - PAD_B);

    /* Data line */
    printf("<path d=\"");
    for (int i = 0; i < npoints; i++) {
        double y = PAD_T + chart_h * (1.0 - (ys[i] - y_lo) / (y_hi - y_lo));
        printf("%c%.1f,%.1f", i == 0 ? 'M' : 'L', xs[i], y);
    }
    printf("\" fill=\"none\" stroke=\"#2196F3\" stroke-width=\"2.5\" "
           "stroke-linejoin=\"round\"/>\n");

    /* Current value dot + label */
    double last_y = PAD_T + chart_h * (1.0 - (ys[npoints-1] - y_lo) / (y_hi - y_lo));
    printf("<circle cx=\"%.0f\" cy=\"%.0f\" r=\"4\" fill=\"#2196F3\"/>\n",
           xs[npoints-1], last_y);
    printf("<text x=\"%.0f\" y=\"%.0f\" text-anchor=\"end\" class=\"l\" "
           "fill=\"#2196F3\">%.1f%%</text>\n",
           xs[npoints-1] - 8, last_y - 8, ys[npoints-1]);

    /* Y label */
    printf("<text x=\"14\" y=\"%d\" text-anchor=\"middle\" "
           "transform=\"rotate(-90,14,%d)\" class=\"l\">"
           "%% Supply Unmoved 1+ Year</text>\n", H / 2, H / 2);

    printf("</svg>\n");

    fprintf(stderr, "Current 1yr+ HODL: %.1f%% of %.0f ZCL\n",
            ys[npoints-1], total_supply);
    return 0;
}
