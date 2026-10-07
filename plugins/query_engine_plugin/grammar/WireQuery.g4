grammar WireQuery;

// Keyword matching never changes the spelling of identifiers or literal token text.
options { caseInsensitive = true; }

// Token names become C++ enumerators and accessor methods in the generated parser, so none may
// spell a system macro: macOS defines TRUE/FALSE (object-like) and MIN/MAX (function-like).

query: SELECT selectItem (COMMA selectItem)* FROM (owner=identifier DOT)? table=identifier
       (OWNER ownerName (COMMA ownerName)*)? (WHERE wherePredicate=predicate)?
       (GROUP BY fieldPath (COMMA fieldPath)*)? (HAVING havingPredicate=predicate)?
       (ORDER BY orderItem (COMMA orderItem)*)? (LIMIT limit=INTEGER)? SEMICOLON? EOF;
selectItem: fieldPath (AS identifier)? | aggregateCall AS identifier | STAR;
aggregateCall: aggregate LPAREN (STAR | fieldPath) RPAREN;
aggregate: COUNT_AGGREGATE | SUM_AGGREGATE | AVG_AGGREGATE | MIN_AGGREGATE | MAX_AGGREGATE;
orderItem: identifier (ASC | DESC)?;
fieldPath: identifier (DOT identifier)*;
predicate: orPredicate;
orPredicate: andPredicate (OR andPredicate)*;
andPredicate: notPredicate (AND notPredicate)*;
notPredicate: NOT notPredicate | predicateAtom;
predicateAtom: LPAREN predicate RPAREN | expression comparison expression | expression IS NOT? NULL_LITERAL;
expression: fieldPath | literal | aggregateCall;
comparison: EQ | NE | LT | LE | GT | GE;
literal: STRING | (PLUS | MINUS)? (DECIMAL | INTEGER) | TRUE_LITERAL | FALSE_LITERAL | NULL_LITERAL;
ownerName: STRING | identifier;
identifier: IDENTIFIER | QUOTED_IDENTIFIER | OWNER;

SELECT: 'SELECT'; FROM: 'FROM'; OWNER: 'OWNER'; WHERE: 'WHERE'; GROUP: 'GROUP'; BY: 'BY';
HAVING: 'HAVING'; ORDER: 'ORDER'; LIMIT: 'LIMIT'; AS: 'AS'; ASC: 'ASC'; DESC: 'DESC';
COUNT_AGGREGATE: 'COUNT'; SUM_AGGREGATE: 'SUM'; AVG_AGGREGATE: 'AVG'; MIN_AGGREGATE: 'MIN'; MAX_AGGREGATE: 'MAX';
NOT: 'NOT'; AND: 'AND'; OR: 'OR'; IS: 'IS'; NULL_LITERAL: 'NULL'; TRUE_LITERAL: 'TRUE'; FALSE_LITERAL: 'FALSE';
LPAREN: '('; RPAREN: ')'; DOT: '.'; COMMA: ','; SEMICOLON: ';'; STAR: '*';
EQ: '='; NE: '!=' | '<>'; LE: '<='; LT: '<'; GE: '>='; GT: '>'; PLUS: '+'; MINUS: '-';
DECIMAL: [0-9]+ '.' [0-9]+;
INTEGER: [0-9]+;
STRING: '\'' ('\'\'' | ~'\'')* '\'';
QUOTED_IDENTIFIER: '"' ('""' | ~'"')* '"';
IDENTIFIER: [a-z_] [a-z0-9_]*;
WHITESPACE: [ \t\r\n]+ -> skip;
