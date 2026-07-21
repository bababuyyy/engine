# Script de varredura automatizada (Linter Textual)
# Acusará quebra de build imediata caso uma regressão dos taskpackets anteriores escape.

if(NOT DEFINED SRC_FILE)
    message(FATAL_ERROR "Variavel interna de teste SRC_FILE nao especificada.")
endif()
if(NOT DEFINED JOBS_FILE)
    message(FATAL_ERROR "Variavel interna de teste JOBS_FILE nao especificada.")
endif()

file(READ "${SRC_FILE}" SRC_CONTENT)
file(READ "${JOBS_FILE}" JOBS_CONTENT)

# --- BASES DE REGRA ---

set(FORBIDDEN_MAIN 
    "dyn_transfer_buf, false"
    "SDL_GPU_FILLMODE_SOLID"
    "const uint8_t* keys"
    "std::rand"
)

set(MANDATORY_MAIN
    "typedef unsigned char BYTE"
    "SDL_WaitForGPUIdle"
)

set(MANDATORY_JOBS
    "memory_order_release"
)

# --- EXECUÇÃO ---

foreach(ITEM IN LISTS FORBIDDEN_MAIN)
    string(FIND "${SRC_CONTENT}" "${ITEM}" FOUND_POS)
    if(FOUND_POS GREATER -1)
        message(FATAL_ERROR "[LINT] FALHA: Padrao EXPRESSAMENTE PROIBIDO encontrado no main.cpp: '${ITEM}'. Voce regrediu uma regra. Abortando.")
    endif()
endforeach()

foreach(ITEM IN LISTS MANDATORY_MAIN)
    string(FIND "${SRC_CONTENT}" "${ITEM}" FOUND_POS)
    if(FOUND_POS EQUAL -1)
        message(FATAL_ERROR "[LINT] FALHA: Padrao OBRIGATORIO perdido no main.cpp: '${ITEM}'. Restaure o bloco exigido.")
    endif()
endforeach()

foreach(ITEM IN LISTS MANDATORY_JOBS)
    string(FIND "${JOBS_CONTENT}" "${ITEM}" FOUND_POS)
    if(FOUND_POS EQUAL -1)
        message(FATAL_ERROR "[LINT] FALHA: Padrao OBRIGATORIO perdido no jobs.h: '${ITEM}'. A barreira de memoria foi rompida. Restaure o block exigido.")
    endif()
endforeach()

message(STATUS "[LINT] PASS: O linter atesta que nao ha regressoes estruturais nos codigos fonte.")