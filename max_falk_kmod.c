#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kthread.h>
#include <linux/sched.h>
//#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>



// List header and struct that holds one word
struct list_head word_list;
struct word_struct {
    struct list_head list;
    int word_len;
    char *word;
};

// Timer to implement 1 word dump per second
#define WAIT_TIME 1000
static struct timer_list my_timer;


// This mutex will be used to lock the entire list
// Locking of individual list elements may be an option 
//                      but is too much effort for this :)
static struct mutex list_lock;
static struct list_head *current_ptr;

/*
 * This method gets called each time the timer runs out
 *
 * It dumps one word from the list to the kernel log if not empty and resets itself
 */
void my_timer_callback(struct timer_list *timer) {

    mutex_lock(&list_lock);

    // If the list is empty reset current_ptr to beginning
    if (list_empty(&word_list)) {
        //printk("TIMER WARNING: List is empty!\n");
        current_ptr = &word_list;
        goto reset;
    }

    // Stop at the end of the list.
    // No need to print everything multiple times 
    if (current_ptr->next == &word_list) {
        //current_ptr = current_ptr->next->next;
        goto reset;
    } else {
        current_ptr = current_ptr->next;
    }

    struct word_struct *temp;
    temp = list_entry(current_ptr, struct word_struct, list);
    if (temp == NULL) {
        printk("TIMER WARNING: Could not find list entry!\n");
        // Eventuell: current_ptr = &word_list
        goto reset;
    }

    printk("TIMER: %s\n", temp->word);

reset:
    mutex_unlock(&list_lock);
    mod_timer(timer, jiffies + msecs_to_jiffies(WAIT_TIME));
    return;
}

// Variables for device and device class
static dev_t my_device_number;
static struct class *my_class;
static struct cdev my_device;

// Module info
#define MODULE_NAME "AVM-Task"
#define MODULE_CLASS "AVM"


/*
 * The read syscall implementation for this module
 */
static ssize_t driver_read(struct file *File, char *user_buffer, size_t count, loff_t *offs) {

    mutex_lock(&list_lock);
    if (list_empty(&word_list)) {
        // printk("AVM READ - List is empty!\n");
        mutex_unlock(&list_lock);
        return 0;
    }
    
    int not_copied, copied, to_copy;
    struct list_head * ptr = word_list.next;
    struct word_struct * word_struc;


    word_struc = list_entry(ptr, struct word_struct, list);
    to_copy = min(word_struc->word_len, count);
    // printk("AVM READ: Attempting to read %s from list.\n", word_struc->word);
    not_copied = copy_to_user(user_buffer, word_struc->word, to_copy);
    copied = word_struc->word_len - not_copied;

    kfree(word_struc->word);
    list_del(ptr);
    mutex_unlock(&list_lock);
    kfree(word_struc);
    return copied;
}

static ssize_t driver_write(struct file *File, const char *user_buffer, size_t count, loff_t *offs) {

    mutex_lock(&list_lock);

    char * temp = kmalloc(sizeof(char) * count+1, GFP_KERNEL);
    if (temp == NULL) {
        printk("AVM WRITE: Could not allocate memory for temp buffer!\n");
        mutex_unlock(&list_lock);
        return -1;
    }
    //printk("AVM WRITE: Allocated temp memory.\n");

    int not_copied, copied;
    not_copied = copy_from_user(temp, user_buffer, count);
    copied = count - not_copied;
    //printk("Copied data from user buffer to temp. %d bytes were copied.\n", copied);

    int start_of_word = -1;
    struct word_struct *new_word_struct;
    int word_len;
    int i;

    for (i = 0; i < copied; i++) {
        // Set start of word if we have no start yet and the current index isnt a space
        if (start_of_word == -1 && temp[i] != ' ') {
            start_of_word = i;
            continue;
        }

        // If we have found the end of a word 
        // TODO: Push Code Bodies to Method. Write once.
        if (start_of_word != -1 && temp[i] == ' ') {
            // Allocate dynamic memory for word struct
            new_word_struct = kmalloc(sizeof(struct word_struct), GFP_KERNEL); 
            if (new_word_struct == NULL) {
                printk("AVM WRITE - Could not allocate memory for new word struct!\n");
                mutex_unlock(&list_lock);
                return start_of_word-1;
            }
            //printk("AVM WRITE 1: Allocated memory for word_struct successfully");

            // Word length without NUL
            word_len = i - start_of_word;
            new_word_struct->word = kmalloc(sizeof(char)*(word_len+1), GFP_KERNEL);
            if (new_word_struct->word == NULL) {
                printk("AVM WRITE - Could not allocate memory for new word!\n");
                kfree(new_word_struct);
                mutex_unlock(&list_lock);
                return start_of_word-1;
            }

        } else if (start_of_word != -1 && i+1 == count) { // Or if we are at the end of the input
            new_word_struct = kmalloc(sizeof(struct word_struct), GFP_KERNEL);
            if (new_word_struct == NULL) {
                printk("AVM WRITE - Could not allocate memory for new word struct!\n");
                mutex_unlock(&list_lock);
                return start_of_word-1;
            }
            //printk("AVM WRITE 2: Allocated memory for word_struct successfully");

            word_len = i - start_of_word;
            new_word_struct->word = kmalloc(sizeof(char)*(word_len), GFP_KERNEL);
            if (new_word_struct->word == NULL) {
                printk("AVM WRITE - Could not allocate memory for new word!\n");
                kfree(new_word_struct);
                mutex_unlock(&list_lock);
                return start_of_word-1;
            }
        } else {
            continue;
        }

        
        // MEMCPY because the temp buffer is in kernel space already
        memcpy(new_word_struct->word, &temp[start_of_word], word_len);
        //not_copied = copy_from_user(new_word_struct->word, &(temp[start_of_word]), word_len);
        new_word_struct->word[word_len] = '\0';
        new_word_struct->word_len = word_len;
        INIT_LIST_HEAD(&new_word_struct->list);
        //printk("Wrote word: %s to new_word_struct! Length: %d, Not Copied: %d\n", new_word_struct->word, new_word_struct->word_len, not_copied);
        list_add_tail(&new_word_struct->list, &word_list);
        start_of_word = -1;
        continue;
    }
    mutex_unlock(&list_lock);
    return count;
}

static int driver_close(struct inode *device_file, struct file *instance) {
    printk("AVM CLOSE - close was called!\n");
    return 0;
}

static int driver_open(struct inode *device_file, struct file *instance) {
    printk("AVM OPEN - open was called!\n");
    return 0;
}

static struct file_operations fops = {.owner = THIS_MODULE,
    .open = driver_open,
    .release = driver_close,
    .read = driver_read,
    .write = driver_write};

/**
 * @brief This function is called, when the module is loaded into the kernel
 */
static int __init ModuleInit(void) {
    printk("Hello, Kernel!\n");

    // Allocate a device number
    if (alloc_chrdev_region(&my_device_number, 0, 1, MODULE_NAME) < 0) {
        printk("Device Nr. could not be allocated!\n");
        return -1;
    }
    printk("read_write - Device Nr. Major: %d, Minor: %d was registered!\n",
            my_device_number >> 20, my_device_number && 0xfffff);

    // Create a device class
    if ((my_class = class_create(THIS_MODULE, MODULE_CLASS)) == NULL) {
        printk("Driver class can not be created!\n");
        goto ClassError;
    }

    // Create device file
    if (device_create(my_class, NULL, my_device_number, NULL, MODULE_NAME) ==
            NULL) {
        printk("Can not create device file!\n");
        goto DeviceError;
    }

    // Initialise device file
    cdev_init(&my_device, &fops);

    // Initialise the list
    INIT_LIST_HEAD(&word_list);

    // Setup the timer
    timer_setup(&my_timer, my_timer_callback, 0);
    mod_timer(&my_timer, jiffies + msecs_to_jiffies(WAIT_TIME));

    // Initialise mutex for locking the list
    mutex_init(&list_lock);

    // Register device to kernel
    if (cdev_add(&my_device, my_device_number, 1) == -1) {
        printk("Registering of device to kernel failed!\n");
        goto AddError;
    }

    return 0;

AddError:
    device_destroy(my_class, my_device_number);
DeviceError:
    class_destroy(my_class);
ClassError:
    unregister_chrdev_region(my_device_number, 1);
    return -1;
}

static void __exit ModuleExit(void) {
    cdev_del(&my_device);
    device_destroy(my_class, my_device_number);
    class_destroy(my_class);
    unregister_chrdev_region(my_device_number, 1);

    del_timer(&my_timer);

    struct list_head *ptr, *next;
    struct word_struct *entry;

    list_for_each_safe(ptr, next, &word_list) {
        entry = list_entry(ptr, struct word_struct, list);
        kfree(entry->word);
        list_del(ptr);
        kfree(entry);
    }

    printk("Goodbye, Kernel!\n");
}

module_init(ModuleInit);
module_exit(ModuleExit);

/* META INFORMATION */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Maximilian Falk");
MODULE_DESCRIPTION("LKM using list, mutex and timer");
