/*
 * SPDX-FileCopyrightText: 2026 ToDoHelper
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file word_default_book.c
 * @brief Compiled-in starter wordbook.
 *
 * Requirement 17.2 still lists the real wordbook source as open, so the device
 * ships a small sample that exercises the whole flow (card, phonetic, meaning,
 * example, grading, review scheduling). Replace it by dropping a real
 * CET4/CET6 JSON at <storage>/todohelper/words/<book>.json - the format is the
 * same and documented in docs/API.md.
 */

#include "word_default_book.h"

const char WORD_DEFAULT_BOOK_JSON[] =
    "{"
    "\"name\":\"CET4 核心词汇（内置示例）\","
    "\"version\":1,"
    "\"words\":["
    "{\"id\":1,\"spelling\":\"abandon\",\"phonetic\":\"/əˈbændən/\","
    "\"meaning\":\"v. 放弃；抛弃；遗弃\",\"example\":\"He abandoned his car in the snow.\"},"
    "{\"id\":2,\"spelling\":\"ability\",\"phonetic\":\"/əˈbɪləti/\","
    "\"meaning\":\"n. 能力；才能\",\"example\":\"She has the ability to solve hard problems.\"},"
    "{\"id\":3,\"spelling\":\"absolute\",\"phonetic\":\"/ˈæbsəluːt/\","
    "\"meaning\":\"adj. 绝对的；完全的\",\"example\":\"I have absolute confidence in her.\"},"
    "{\"id\":4,\"spelling\":\"academic\",\"phonetic\":\"/ˌækəˈdemɪk/\","
    "\"meaning\":\"adj. 学术的；学院的\",\"example\":\"The academic year starts in September.\"},"
    "{\"id\":5,\"spelling\":\"accept\",\"phonetic\":\"/əkˈsept/\","
    "\"meaning\":\"v. 接受；同意\",\"example\":\"She accepted the invitation.\"},"
    "{\"id\":6,\"spelling\":\"access\",\"phonetic\":\"/ˈækses/\","
    "\"meaning\":\"n. 通道；访问  v. 访问\",\"example\":\"Students have free access to the library.\"},"
    "{\"id\":7,\"spelling\":\"accident\",\"phonetic\":\"/ˈæksɪdənt/\","
    "\"meaning\":\"n. 事故；意外\",\"example\":\"The accident was caused by ice on the road.\"},"
    "{\"id\":8,\"spelling\":\"accompany\",\"phonetic\":\"/əˈkʌmpəni/\","
    "\"meaning\":\"v. 陪伴；伴随\",\"example\":\"Thunder usually accompanies lightning.\"},"
    "{\"id\":9,\"spelling\":\"achieve\",\"phonetic\":\"/əˈtʃiːv/\","
    "\"meaning\":\"v. 达到；完成；实现\",\"example\":\"He achieved his goal in two years.\"},"
    "{\"id\":10,\"spelling\":\"acquire\",\"phonetic\":\"/əˈkwaɪə(r)/\","
    "\"meaning\":\"v. 获得；取得；学到\",\"example\":\"She acquired a good knowledge of Chinese.\"},"
    "{\"id\":11,\"spelling\":\"adapt\",\"phonetic\":\"/əˈdæpt/\","
    "\"meaning\":\"v. 适应；改编\",\"example\":\"It takes time to adapt to a new city.\"},"
    "{\"id\":12,\"spelling\":\"adequate\",\"phonetic\":\"/ˈædɪkwət/\","
    "\"meaning\":\"adj. 足够的；适当的\",\"example\":\"The room is adequate for two people.\"},"
    "{\"id\":13,\"spelling\":\"adjust\",\"phonetic\":\"/əˈdʒʌst/\","
    "\"meaning\":\"v. 调整；适应\",\"example\":\"Please adjust the screen brightness.\"},"
    "{\"id\":14,\"spelling\":\"admire\",\"phonetic\":\"/ədˈmaɪə(r)/\","
    "\"meaning\":\"v. 钦佩；欣赏\",\"example\":\"I admire her patience.\"},"
    "{\"id\":15,\"spelling\":\"adopt\",\"phonetic\":\"/əˈdɒpt/\","
    "\"meaning\":\"v. 采用；收养\",\"example\":\"The team adopted a new strategy.\"},"
    "{\"id\":16,\"spelling\":\"advance\",\"phonetic\":\"/ədˈvɑːns/\","
    "\"meaning\":\"n. 前进；进步  v. 前进；提前\",\"example\":\"Medical science advances quickly.\"},"
    "{\"id\":17,\"spelling\":\"advantage\",\"phonetic\":\"/ədˈvɑːntɪdʒ/\","
    "\"meaning\":\"n. 优势；有利条件\",\"example\":\"Speaking two languages is an advantage.\"},"
    "{\"id\":18,\"spelling\":\"affect\",\"phonetic\":\"/əˈfekt/\","
    "\"meaning\":\"v. 影响；感动\",\"example\":\"Weather affects our mood.\"},"
    "{\"id\":19,\"spelling\":\"afford\",\"phonetic\":\"/əˈfɔːd/\","
    "\"meaning\":\"v. 买得起；负担得起\",\"example\":\"We cannot afford a new car this year.\"},"
    "{\"id\":20,\"spelling\":\"aggressive\",\"phonetic\":\"/əˈɡresɪv/\","
    "\"meaning\":\"adj. 好斗的；积极进取的\",\"example\":\"They took an aggressive sales approach.\"},"
    "{\"id\":21,\"spelling\":\"agriculture\",\"phonetic\":\"/ˈæɡrɪkʌltʃə(r)/\","
    "\"meaning\":\"n. 农业\",\"example\":\"Agriculture is important to this region.\"},"
    "{\"id\":22,\"spelling\":\"alternative\",\"phonetic\":\"/ɔːlˈtɜːnətɪv/\","
    "\"meaning\":\"n. 替代方案  adj. 可供替代的\",\"example\":\"Is there an alternative to this plan?\"},"
    "{\"id\":23,\"spelling\":\"amount\",\"phonetic\":\"/əˈmaʊnt/\","
    "\"meaning\":\"n. 数量；总额  v. 合计\",\"example\":\"A large amount of data was collected.\"},"
    "{\"id\":24,\"spelling\":\"analyze\",\"phonetic\":\"/ˈænəlaɪz/\","
    "\"meaning\":\"v. 分析\",\"example\":\"Let us analyze the results together.\"},"
    "{\"id\":25,\"spelling\":\"ancient\",\"phonetic\":\"/ˈeɪnʃənt/\","
    "\"meaning\":\"adj. 古代的；古老的\",\"example\":\"They studied ancient Chinese history.\"},"
    "{\"id\":26,\"spelling\":\"anxious\",\"phonetic\":\"/ˈæŋkʃəs/\","
    "\"meaning\":\"adj. 焦虑的；渴望的\",\"example\":\"She was anxious about the exam.\"},"
    "{\"id\":27,\"spelling\":\"apparent\",\"phonetic\":\"/əˈpærənt/\","
    "\"meaning\":\"adj. 明显的；表面上的\",\"example\":\"It became apparent that he was right.\"},"
    "{\"id\":28,\"spelling\":\"appeal\",\"phonetic\":\"/əˈpiːl/\","
    "\"meaning\":\"v./n. 呼吁；上诉；吸引力\",\"example\":\"The design appeals to young users.\"},"
    "{\"id\":29,\"spelling\":\"appreciate\",\"phonetic\":\"/əˈpriːʃieɪt/\","
    "\"meaning\":\"v. 感激；欣赏；理解\",\"example\":\"I really appreciate your help.\"},"
    "{\"id\":30,\"spelling\":\"approach\",\"phonetic\":\"/əˈprəʊtʃ/\","
    "\"meaning\":\"n. 方法；接近  v. 接近\",\"example\":\"We need a different approach.\"},"
    "{\"id\":31,\"spelling\":\"appropriate\",\"phonetic\":\"/əˈprəʊpriət/\","
    "\"meaning\":\"adj. 适当的；恰当的\",\"example\":\"Wear appropriate clothes for the weather.\"},"
    "{\"id\":32,\"spelling\":\"approve\",\"phonetic\":\"/əˈpruːv/\","
    "\"meaning\":\"v. 批准；赞成\",\"example\":\"The board approved the budget.\"}"
    "]"
    "}";
