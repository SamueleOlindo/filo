#pragma once

namespace filo {

// Test bench for semantic search in Italian.
//
// Every query is built around a real LEXICAL GAP: the word searched for does
// NOT appear in the passages that match it. That is exactly the case where
// word matching fails and semantic search has to work — a model that passes
// only because it found the same words again is no use to us.
//
// The distractors are not random: many of them sit in the same domain as the
// query (contracts, invoices, deadlines) and differ only in meaning. A bench
// made of sentences far apart from each other would hand every model a high
// score and would tell no two of them apart.

struct EvalPassage {
    const char* text;
    int topic;   // same number as the query it answers, -1 = distractor
};

struct EvalQuery {
    const char* text;
    int topic;
};

inline constexpr EvalQuery kEvalQueries[] = {
    {"penale per la consegna in ritardo", 0},
    {"come faccio a disdire l'abbonamento", 1},
    {"quanto devo versare all'inizio", 2},
    {"licenziare un dipendente", 3},
    {"il documento non ha valore fiscale", 4},
    {"chi paga se il prodotto si rompe", 5},
    {"dati personali e privacy", 6},
    {"rimborso delle spese di viaggio", 7},
    {"scadenza per contestare", 8},
    {"aumento del canone annuale", 9},
};

inline constexpr EvalPassage kEvalPassages[] = {
    // 0 — penalty / late delivery
    {"in caso di mancato rispetto dei termini si applica una clausola risarcitoria", 0},
    {"il fornitore corrispondera' un importo forfettario per ogni giorno eccedente", 0},
    {"e' prevista una sanzione pecuniaria proporzionata al valore dell'ordine", 0},
    {"the supplier shall pay liquidated damages for each day of delay", 0},

    // 1 — withdrawal / cancellation
    {"la parte puo' esercitare il diritto di recesso con preavviso di trenta giorni", 1},
    {"per interrompere il servizio e' sufficiente una comunicazione scritta", 1},
    {"il rapporto si scioglie automaticamente se non viene rinnovato", 1},

    // 2 — down payment / deposit
    {"alla firma e' dovuto un deposito cauzionale pari al dieci per cento", 2},
    {"l'anticipo verra' scomputato dall'ultima rata", 2},
    {"e' richiesto un versamento iniziale a titolo di garanzia", 2},

    // 3 — dismissal
    {"la risoluzione del rapporto di lavoro richiede giusta causa documentata", 3},
    {"il datore puo' interrompere il contratto solo per motivi oggettivi", 3},
    {"cessazione anticipata del rapporto per giustificato motivo", 3},

    // 4 — courtesy copy
    {"il presente documento e' una copia di cortesia priva di valenza fiscale", 4},
    {"l'originale con valore legale e' stato trasmesso al sistema di interscambio", 4},

    // 5 — warranty / defects
    {"i vizi di produzione sono coperti per ventiquattro mesi dalla consegna", 5},
    {"il venditore risponde della conformita' del bene", 5},
    {"la copertura non si applica ai danni causati da uso improprio", 5},

    // 6 — data protection
    {"il trattamento delle informazioni avviene nel rispetto del regolamento europeo", 6},
    {"l'interessato puo' chiedere la cancellazione delle proprie informazioni", 6},
    {"i dati non saranno comunicati a terzi senza consenso esplicito", 6},

    // 7 — travel expenses
    {"le spese di trasferta sono riconosciute dietro presentazione dei giustificativi", 7},
    {"e' previsto un indennizzo chilometrico per gli spostamenti con mezzo proprio", 7},

    // 8 — deadline to raise a complaint
    {"eventuali contestazioni vanno sollevate entro otto giorni dal ricevimento", 8},
    {"decorso tale periodo la fornitura si intende accettata senza riserve", 8},

    // 9 — fee increase
    {"l'importo periodico e' soggetto a rivalutazione secondo l'indice ISTAT", 9},
    {"la quota mensile puo' essere aggiornata una volta l'anno", 9},

    // SAME-DOMAIN distractors: this is where the difference shows
    {"il pagamento avviene tramite bonifico bancario entro trenta giorni", -1},
    {"la fattura riporta l'aliquota ordinaria del ventidue per cento", -1},
    {"il foro competente per ogni controversia e' quello di Milano", -1},
    {"le parti eleggono domicilio presso le rispettive sedi legali", -1},
    {"l'accordo e' redatto in duplice copia di pari valore", -1},
    {"la merce viaggia a rischio e pericolo del destinatario", -1},
    {"il codice fiscale e la partita IVA sono riportati in intestazione", -1},
    {"l'ordine si intende confermato alla ricezione del presente documento", -1},

    // far-off distractors: they check the model does not flatten everything
    {"soffriggere la cipolla in padella con un filo d'olio", -1},
    {"per installare le dipendenze esegui il comando nella cartella del progetto", -1},
    {"le previsioni indicano pioggia debole sulla costa adriatica", -1},
    {"la partita si e' conclusa con un pareggio all'ultimo minuto", -1},
    {"il volo parte dal terminal tre alle sette del mattino", -1},
};

inline constexpr int kEvalQueryCount =
    static_cast<int>(sizeof(kEvalQueries) / sizeof(kEvalQueries[0]));
inline constexpr int kEvalPassageCount =
    static_cast<int>(sizeof(kEvalPassages) / sizeof(kEvalPassages[0]));

}  // namespace filo
