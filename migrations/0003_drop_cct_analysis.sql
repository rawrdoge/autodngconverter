-- CCT analysis upgrade path (PRD_CCT_Analysis_Native_Cpp_v2.3.0 §9.1).
-- Harmless no-op for main-deployed databases (0002 never reached main).
-- Correct cleanup for anyone who ran a feat/cct-plugin image locally.
DROP TABLE IF EXISTS cct_results;
