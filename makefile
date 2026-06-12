MODULE_big = pg_eatrace
OBJDIR = obj
OBJS = $(OBJDIR)/pg_eatrace.o $(OBJDIR)/pg_eatrace_error.o $(OBJDIR)/pg_eatrace_http.o $(OBJDIR)/pg_eatrace_otlp_json.o $(OBJDIR)/pg_eatrace_plan.o $(OBJDIR)/pg_eatrace_planner.o $(OBJDIR)/pg_eatrace_query_span.o $(OBJDIR)/pg_eatrace_queue.o $(OBJDIR)/pg_eatrace_span.o $(OBJDIR)/pg_eatrace_status.o $(OBJDIR)/pg_eatrace_trace_context.o $(OBJDIR)/pg_eatrace_trace_state.o $(OBJDIR)/pg_eatrace_utility_span.o $(OBJDIR)/pg_eatrace_worker.o
EXTENSION = pg_eatrace
DATA = pg_eatrace--0.0.sql
EXTRA_CLEAN = $(OBJDIR)/*.Po $(OBJDIR)/otlp_json_test
PG_CPPFLAGS = -Iinclude
PG_CONFIG = pg_config
PG_INCLUDEDIR_SERVER = $(shell $(PG_CONFIG) --includedir-server)
PG_LIBDIR = $(shell $(PG_CONFIG) --libdir)
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(COMPILE.c) -o $@ $< -MMD -MP -MF $(OBJDIR)/$(*F).Po

$(OBJDIR)/%.bc: src/%.c | $(OBJDIR)
	$(COMPILE.c.bc) -o $@ $<

-include $(wildcard $(OBJDIR)/*.Po)

$(OBJDIR)/otlp_json_test: test/otlp_json_test.c src/pg_eatrace_span.c src/pg_eatrace_otlp_json.c src/pg_eatrace_trace_context.c | $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PG_CPPFLAGS) -I$(PG_INCLUDEDIR_SERVER) -o $@ $^ -L$(PG_LIBDIR) -lpgcommon -lpgport -lm

.PHONY: check-otlp-json
check-otlp-json: $(OBJDIR)/otlp_json_test
	$<

check: check-otlp-json
installcheck: check-otlp-json
