%{
#define YYDEBUG 1

#include <stdio.h>

#include <iostream>
#include <string>

#include "searchdata.h"
#include "wasaparserdriver.h"
#include "wasaparse.tab.h"

using namespace std;

int yylex(yy::parser::semantic_type *, WasaParserDriver *);
void yyerror(char const *);
static void qualify(Rcl::SearchDataClauseDist *, const string &);

static void addSubQuery(WasaParserDriver *d,
                        Rcl::SearchData *sd, Rcl::SearchData *sq)
{
    sd->addClause(new Rcl::SearchDataClauseSub(RefCntr<Rcl::SearchData>(sq)));
}

%}

%skeleton "lalr1.cc"
%defines
%error-verbose

%parse-param {WasaParserDriver* d}
%lex-param {WasaParserDriver* d}

%union {
    std::string *str;
    Rcl::SearchDataClauseSimple *cl;
    Rcl::SearchData *sd;
}
%destructor {delete $$;} <str>

%type <cl> qualquote
%type <cl> fieldexpr
%type <cl> term
%type <sd> query
%type <str> complexfieldname

 /* Non operator tokens need precedence because of the possibility of
    concatenation which needs to have lower prec than OR */
%left <str> WORD
%left <str> QUOTED
%left <str> QUALIFIERS
%left AND UCONCAT
%left OR

%token EQUALS CONTAINS SMALLEREQ SMALLER GREATEREQ GREATER

%%

topquery: query
{
    d->m_result = $1;
}

query: 
query query %prec UCONCAT
{
    //cerr << "q: query query" << endl;
    Rcl::SearchData *sd = new Rcl::SearchData(Rcl::SCLT_AND, d->m_stemlang);
    addSubQuery(d, sd, $1);
    addSubQuery(d, sd, $2);
    $$ = sd;
}
| query AND query
{
    //cerr << "q: query AND query" << endl;
    Rcl::SearchData *sd = new Rcl::SearchData(Rcl::SCLT_AND, d->m_stemlang);
    addSubQuery(d, sd, $1);
    addSubQuery(d, sd, $3);
    $$ = sd;
}
| query OR query
{
    //cerr << "q: query OR query" << endl;
    Rcl::SearchData *top = new Rcl::SearchData(Rcl::SCLT_AND, d->m_stemlang);
    Rcl::SearchData *sd = new Rcl::SearchData(Rcl::SCLT_OR, d->m_stemlang);
    addSubQuery(d, sd, $1);
    addSubQuery(d, sd, $3);
    addSubQuery(d, top, sd);
    $$ = top;
}
| '(' query ')' 
{
    //cerr << "q: ( query )" << endl;
    $$ = $2;
}
|
fieldexpr %prec UCONCAT
{
    //cerr << "q: fieldexpr" << endl;
    Rcl::SearchData *sd = new Rcl::SearchData(Rcl::SCLT_AND, d->m_stemlang);
    d->addClause(sd, $1);
    $$ = sd;
}
;

fieldexpr: term 
{
    // cerr << "fe: simple fieldexpr: " << $1->gettext() << endl;
    $$ = $1;
}
| complexfieldname EQUALS term 
{
    // cerr << "fe: " << *$1 << " = " << $3->gettext() << endl;
    $3->setfield(*$1);
    $3->setrel(Rcl::SearchDataClause::REL_EQUALS);
    $$ = $3;
    delete $1;
}
| complexfieldname CONTAINS term 
{
    // cerr << "fe: " << *$1 << " : " << $3->gettext() << endl;
    $3->setfield(*$1);
    $3->setrel(Rcl::SearchDataClause::REL_CONTAINS);
    $$ = $3;
    delete $1;
}
| complexfieldname SMALLER term 
{
    // cerr << "fe: " << *$1 << " < " << $3->gettext() << endl;
    $3->setfield(*$1);
    $3->setrel(Rcl::SearchDataClause::REL_LT);
    $$ = $3;
    delete $1;
}
| complexfieldname SMALLEREQ term 
{
    // cerr << "fe: " << *$1 << " <= " << $3->gettext() << endl;
    $3->setfield(*$1);
    $3->setrel(Rcl::SearchDataClause::REL_LTE);
    $$ = $3;
    delete $1;
}
| complexfieldname GREATER term 
{
    // cerr << "fe: "  << *$1 << " > " << $3->gettext() << endl;
    $3->setfield(*$1);
    $3->setrel(Rcl::SearchDataClause::REL_GT);
    $$ = $3;
    delete $1;
}
| complexfieldname GREATEREQ term 
{
    // cerr << "fe: " << *$1 << " >= " << $3->gettext() << endl;
    $3->setfield(*$1);
    $3->setrel(Rcl::SearchDataClause::REL_GTE);
    $$ = $3;
    delete $1;
}
| '-' fieldexpr 
{
    // cerr << "fe: - fieldexpr[" << $2->gettext() << "]" << endl;
    $2->setexclude(true);
    $$ = $2;
}
;

/* Deal with field names like dc:title */
complexfieldname: 
WORD
{
    // cerr << "cfn: WORD" << endl;
    $$ = $1;
}
|
complexfieldname CONTAINS WORD
{
    // cerr << "cfn: complexfieldname ':' WORD" << endl;
    $$ = new string(*$1 + string(":") + *$3);
    delete $1;
    delete $3;
}

term: 
WORD
{
    //cerr << "term[" << *$1 << "]" << endl;
    $$ = new Rcl::SearchDataClauseSimple(Rcl::SCLT_AND, *$1);
    delete $1;
}
| qualquote 
{
    $$ = $1;
}

qualquote: 
QUOTED
{
    // cerr << "QUOTED[" << *$1 << "]" << endl;
    $$ = new Rcl::SearchDataClauseDist(Rcl::SCLT_PHRASE, *$1, 0);
    delete $1;
}
| QUOTED QUALIFIERS 
{
    // cerr << "QUOTED[" << *$1 << "] QUALIFIERS[" << *$2 << "]" << endl;
    Rcl::SearchDataClauseDist *cl = 
        new Rcl::SearchDataClauseDist(Rcl::SCLT_PHRASE, *$1, 0);
    qualify(cl, *$2);
    $$ = cl;
    delete $1;
    delete $2;
}


%%

#include <ctype.h>

// Look for int at index, skip and return new index found? value.
static unsigned int qualGetInt(const string& q, unsigned int cur, int *pval)
{
    unsigned int ncur = cur;
    if (cur < q.size() - 1) {
        char *endptr;
        int val = strtol(&q[cur + 1], &endptr, 10);
        if (endptr != &q[cur + 1]) {
            ncur += endptr - &q[cur + 1];
            *pval = val;
        }
    }
    return ncur;
}

static void qualify(Rcl::SearchDataClauseDist *cl, const string& quals)
{
    // cerr << "qualify(" << cl << ", " << quals << ")" << endl;
    for (unsigned int i = 0; i < quals.length(); i++) {
        //fprintf(stderr, "qual char %c\n", quals[i]);
        switch (quals[i]) {
        case 'b': 
            cl->setWeight(10.0);
            break;
        case 'c': break;
        case 'C': 
            cl->addModifier(Rcl::SearchDataClause::SDCM_CASESENS);
            break;
        case 'd': break;
        case 'D':  
            cl->addModifier(Rcl::SearchDataClause::SDCM_DIACSENS);
            break;
        case 'e': 
            cl->addModifier(Rcl::SearchDataClause::SDCM_CASESENS);
            cl->addModifier(Rcl::SearchDataClause::SDCM_DIACSENS);
            cl->addModifier(Rcl::SearchDataClause::SDCM_NOSTEMMING);
            break;
        case 'l': 
            cl->addModifier(Rcl::SearchDataClause::SDCM_NOSTEMMING);
            break;
        case 'L': break;
        case 'o':  
        {
            int slack = 10;
            i = qualGetInt(quals, i, &slack);
            cl->setslack(slack);
            //cerr << "set slack " << cl->getslack() << " done" << endl;
        }
        break;
        case 'p': 
            cl->setTp(Rcl::SCLT_NEAR);
            if (cl->getslack() == 0) {
                cl->setslack(10);
                //cerr << "set slack " << cl->getslack() << " done" << endl;
            }
            break;
        case '.':case '0':case '1':case '2':case '3':case '4':
        case '5':case '6':case '7':case '8':case '9':
        {
            int n = 0;
            float factor = 1.0;
            if (sscanf(&(quals[i]), "%f %n", &factor, &n)) {
                if (factor != 1.0) {
                    cl->setWeight(factor);
                }
            }
            if (n > 0)
                i += n - 1;
        }
        default:
            break;
        }
    }
}


// specialstartchars are special only at the beginning of a token
// (e.g. doctor-who is a term, not 2 terms separated by '-')
static const string specialstartchars("-");
// specialinchars are special everywhere except inside a quoted string
static const string specialinchars(":=<>()");

// Called with the first dquote already read
static int parseString(WasaParserDriver *d, yy::parser::semantic_type *yylval)
{
    string* value = new string();
    d->qualifiers().clear();
    int c;
    while ((c = d->GETCHAR())) {
        switch (c) {
        case '\\':
            /* Escape: get next char */
            c = d->GETCHAR();
            if (c == 0) {
                value->push_back(c);
                goto out;
            }
            value->push_back(c);
            break;
        case '"':
            /* End of string. Look for qualifiers */
            while ((c = d->GETCHAR()) && !isspace(c))
                d->qualifiers().push_back(c);
            goto out;
        default:
            value->push_back(c);
        }
    }
out:
    //cerr << "GOT QUOTED ["<<value<<"] quals [" << d->qualifiers() << "]" << endl;
    yylval->str = value;
    return yy::parser::token::QUOTED;
}


int yylex(yy::parser::semantic_type *yylval, WasaParserDriver *d)
{
    if (!d->qualifiers().empty()) {
        yylval->str = new string();
        yylval->str->swap(d->qualifiers());
        return yy::parser::token::QUALIFIERS;
    }

    int c;

    /* Skip white space.  */
    while ((c = d->GETCHAR()) && isspace(c))
        continue;

    if (c == 0)
        return 0;

    if (specialstartchars.find_first_of(c) != string::npos) {
        //cerr << "yylex: return " << c << endl;
        return c;
    }

    // field-term relations
    switch (c) {
    case '=': return yy::parser::token::EQUALS;
    case ':': return yy::parser::token::CONTAINS;
    case '<': {
        int c1 = d->GETCHAR();
        if (c1 == '=') {
            return yy::parser::token::SMALLEREQ;
        } else {
            d->UNGETCHAR(c1);
            return yy::parser::token::SMALLER;
        }
    }
    case '>': {
        int c1 = d->GETCHAR();
        if (c1 == '=') {
            return yy::parser::token::GREATEREQ;
        } else {
            d->UNGETCHAR(c1);
            return yy::parser::token::GREATER;
        }
    }
    case '(': case ')':
        return c;
    }
        
    if (c == '"')
        return parseString(d, yylval);

    d->UNGETCHAR(c);

    // Other chars start a term or field name or reserved word
    string* word = new string();
    while ((c = d->GETCHAR())) {
        if (isspace(c)) {
            //cerr << "Word broken by whitespace" << endl;
            break;
        } else if (specialinchars.find_first_of(c) != string::npos) {
            //cerr << "Word broken by special char" << endl;
            d->UNGETCHAR(c);
            break;
        } else if (c == 0) {
            //cerr << "Word broken by EOF" << endl;
            break;
        } else {
            word->push_back(c);
        }
    }
    
    if (!word->compare("AND") || !word->compare("&&")) {
        delete word;
        return yy::parser::token::AND;
    } else if (!word->compare("OR") || !word->compare("||")) {
        delete word;
        return yy::parser::token::OR;
    }

//    cerr << "Got word [" << word << "]" << endl;
    yylval->str = word;
    return yy::parser::token::WORD;
}