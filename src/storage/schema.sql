CREATE TABLE schema_meta (
    version INTEGER NOT NULL CHECK (version = 2)
);
INSERT INTO schema_meta(version) VALUES (2);

CREATE TABLE runs (
    id TEXT PRIMARY KEY,
    draft INTEGER NOT NULL CHECK (draft IN (18, 21)),
    transport INTEGER NOT NULL CHECK (transport IN (0, 1)),
    mode INTEGER NOT NULL CHECK (mode IN (0, 1)),
    timeout_ms INTEGER NOT NULL CHECK (timeout_ms >= 0),
    state INTEGER NOT NULL CHECK (state IN (0, 1)),
    created_at_unix_ns INTEGER NOT NULL,
    finalized_at_unix_ns INTEGER,
    CHECK ((state = 0 AND finalized_at_unix_ns IS NULL) OR
           (state = 1 AND finalized_at_unix_ns IS NOT NULL AND
            finalized_at_unix_ns > created_at_unix_ns))
);

CREATE TABLE run_builds (
    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON DELETE CASCADE,
    version TEXT NOT NULL,
    source_revision TEXT NOT NULL
);

CREATE TABLE build_dependencies (
    run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    revision TEXT NOT NULL,
    PRIMARY KEY (run_id, name)
);

CREATE TABLE selected_scenarios (
    run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position >= 0),
    scenario_id TEXT NOT NULL,
    PRIMARY KEY (run_id, position)
);

CREATE TABLE run_track_fixtures (
    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON DELETE CASCADE,
    track_name TEXT NOT NULL
);

CREATE TABLE run_track_namespace_fields (
    run_id TEXT NOT NULL REFERENCES run_track_fixtures(run_id) ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position >= 0),
    value TEXT NOT NULL,
    PRIMARY KEY (run_id, position)
);

CREATE TABLE evidence_events (
    run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    sequence INTEGER NOT NULL CHECK (sequence >= 0),
    monotonic_time_ns INTEGER NOT NULL,
    wall_time_unix_ns INTEGER NOT NULL,
    kind TEXT NOT NULL,
    detail TEXT NOT NULL,
    connection_id TEXT,
    stream_id TEXT,
    request_id TEXT,
    scenario_id TEXT,
    requirement_id TEXT,
    PRIMARY KEY (run_id, sequence)
);

CREATE TABLE final_scores (
    run_id TEXT PRIMARY KEY REFERENCES runs(id) ON DELETE CASCADE,
    verdict INTEGER NOT NULL CHECK (verdict BETWEEN 0 AND 3),
    required_earned TEXT NOT NULL,
    required_possible TEXT NOT NULL,
    weighted_earned TEXT NOT NULL,
    weighted_possible TEXT NOT NULL,
    coverage_earned TEXT NOT NULL,
    coverage_possible TEXT NOT NULL
);

CREATE TABLE outcomes (
    run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
    position INTEGER NOT NULL CHECK (position >= 0),
    requirement_id TEXT NOT NULL,
    state INTEGER NOT NULL CHECK (state BETWEEN 0 AND 4),
    PRIMARY KEY (run_id, requirement_id),
    UNIQUE (run_id, position)
);

CREATE INDEX runs_newest_idx ON runs(created_at_unix_ns DESC, id DESC);
CREATE INDEX evidence_events_page_idx ON evidence_events(run_id, sequence);
