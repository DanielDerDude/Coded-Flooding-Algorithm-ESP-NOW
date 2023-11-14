/* This header contains my own API for a doubly linked list. This will be used to store the peer lists, packet pools etc. */

#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#endif

static const char *LIST_TAG = "timer";

// xNode structure
typedef struct xNode {
    void* xData;
    int64_t xItemValue;
    struct xNode* pxNext;
    struct xNode* pxPrev;
};

// list structure
typedef struct DoubleLinkedList_t {
    uint8_t length;
    xNode* pxHead;
    xNode* pxTail;
} DoubleLinkedList_t;

/* Function to initialise the List */
void vInitList(DoubleLinkedList_t* list) {    
    list->pxHead = NULL;        // points to first element
    list->pxTail = NULL;        // points to last element
}

// insert a new element in sorted order based on xItemValue (High -> Low)
void vInsertSorted(DoubleLinkedList_t* list, void* data, size_t dataSize, int64_t itemValue) {
    xNode* newNode = (xNode*)malloc(sizeof(xNode));
    if (newNode == NULL){
        ESP_LOGE(LIST_TAG, "Memory allocation faild for new Node")
    }
    newNode->xData = malloc(dataSize);
    newNode->xData = 
    memcpy(newNode->xData, data, dataSize);

    newNode->xItemValue = itemValue;
    newNode->pxNext = NULL;
    newNode->pxPrev = NULL;

    xNode* current = list->pxHead;
    while (current != NULL && current->xItemValue < itemValue) {
        current = current->pxNext;
    }

    if (current == NULL) {      // new node at the end
        newNode->pxPrev = list->pxTail;
        list->pxTail->pxNext = newNode;
        list->pxTail = newNode;
    } else {
        // Insert before current
        newNode->pxPrev = current->pxPrev;
        newNode->pxNext = current;
        if (current->pxPrev != NULL) {
            current->pxPrev->pxNext = newxNode;
        } else {
            list->pxHead = newxNode;
        }
        current->pxPrev = newxNode;
    }
}

// return data of a node, needs to be casted to the type of original data.
void* xGetHeadData(DoubleLinkedList_t* list){

}

// Display the elements in the list
void displayList(DoubleLinkedList_t* list, void (*printFn)(void*)) {
    xNode* current = list->pxHead;
    while (current != NULL) {
        printFn(current->xData);
        current = current->pxNext;
    }
}

void vDeleteList(DoubleLinkedList_t* list){
    // todo
}